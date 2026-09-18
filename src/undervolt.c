#define _POSIX_C_SOURCE 200809L

#include "undervolt.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *UNDERVOLT_DOMAIN_NAMES[UNDERVOLT_DOMAIN_COUNT] = {
    "CPU",
    "GPU",
    "CPU Cache",
    "Uncore",
    "Analog I/O",
};

/* ------------------------------------------------------------------------- */
/* Utility helpers                                                            */
/* ------------------------------------------------------------------------- */

static void set_error(char *buffer, size_t size, const char *message) {
    if (buffer != NULL && size > 0) {
        snprintf(buffer, size, "%s", message);
    }
}

static const char *skip_space(const char *text) {
    while (text != NULL && isspace((unsigned char)*text)) {
        ++text;
    }
    return text;
}

/*
 * Parse just enough of an intel-undervolt line to obtain its domain index.
 * Display names are intentionally ignored because they may contain spaces.
 */
static int parse_domain_index(const char *line, int *index) {
    const char *cursor = skip_space(line);
    const char keyword[] = "undervolt";

    if (strncmp(cursor, keyword, sizeof(keyword) - 1U) != 0 ||
        !isspace((unsigned char)cursor[sizeof(keyword) - 1U])) {
        return 0;
    }

    cursor = skip_space(cursor + sizeof(keyword) - 1U);
    char *end = NULL;
    errno = 0;
    long parsed = strtol(cursor, &end, 10);
    if (cursor == end || errno != 0 || parsed < 0 || parsed >= UNDERVOLT_DOMAIN_COUNT) {
        return -1;
    }

    *index = (int)parsed;
    return 1;
}

/* The offset is the final whitespace-separated token on an undervolt line. */
static int parse_last_double(const char *line, double *value) {
    const char *end = line + strlen(line);
    while (end > line && isspace((unsigned char)end[-1])) {
        --end;
    }

    const char *start = end;
    while (start > line && !isspace((unsigned char)start[-1])) {
        --start;
    }
    if (start == end) {
        return -1;
    }

    char number[64];
    size_t length = (size_t)(end - start);
    if (length >= sizeof(number)) {
        return -1;
    }
    memcpy(number, start, length);
    number[length] = '\0';

    char *number_end = NULL;
    errno = 0;
    double parsed = strtod(number, &number_end);
    if (number == number_end || *number_end != '\0' || errno != 0 || !isfinite(parsed)) {
        return -1;
    }

    *value = parsed;
    return 0;
}

static int write_domain_line(FILE *file, int index, double offset_mv) {
    return fprintf(file,
                   "undervolt %d '%s' %.2f\n",
                   index,
                   UNDERVOLT_DOMAIN_NAMES[index],
                   offset_mv) >= 0
               ? 0
               : -1;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                 */
/* ------------------------------------------------------------------------- */

void undervolt_defaults(double offsets[UNDERVOLT_DOMAIN_COUNT]) {
    if (offsets == NULL) {
        return;
    }
    for (size_t index = 0; index < UNDERVOLT_DOMAIN_COUNT; ++index) {
        offsets[index] = 0.0;
    }
}

int undervolt_validate(const double offsets[UNDERVOLT_DOMAIN_COUNT],
                       char *error,
                       size_t error_size) {
    if (offsets == NULL) {
        set_error(error, error_size, "Offsets de tensão ausentes.");
        return -1;
    }

    for (size_t index = 0; index < UNDERVOLT_DOMAIN_COUNT; ++index) {
        if (!isfinite(offsets[index]) ||
            offsets[index] < UNDERVOLT_MIN_MV ||
            offsets[index] > UNDERVOLT_MAX_MV) {
            if (error != NULL && error_size > 0) {
                snprintf(error,
                         error_size,
                         "%s precisa ficar entre %.0f e %.0f mV.",
                         UNDERVOLT_DOMAIN_NAMES[index],
                         UNDERVOLT_MIN_MV,
                         UNDERVOLT_MAX_MV);
            }
            return -1;
        }
    }
    return 0;
}

int undervolt_load_config(const char *path,
                          double offsets[UNDERVOLT_DOMAIN_COUNT],
                          char *error,
                          size_t error_size) {
    if (path == NULL || offsets == NULL) {
        set_error(error, error_size, "Parâmetros inválidos ao ler o undervolt.");
        return -1;
    }

    undervolt_defaults(offsets);

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        if (errno == ENOENT) {
            return 0;
        }
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size, "Não foi possível abrir %s: %s", path, strerror(errno));
        }
        return -1;
    }

    char *line = NULL;
    size_t capacity = 0;
    ssize_t length = 0;
    int result = 0;

    while ((length = getline(&line, &capacity, file)) >= 0) {
        (void)length;
        int index = -1;
        int parsed = parse_domain_index(line, &index);
        if (parsed <= 0) {
            continue;
        }

        double offset = 0.0;
        if (parse_last_double(line, &offset) != 0) {
            set_error(error, error_size, "Linha de undervolt inválida no arquivo de configuração.");
            result = -1;
            break;
        }
        offsets[index] = offset;
    }

    free(line);
    fclose(file);
    return result;
}

int undervolt_write_config_copy(const char *source,
                                const char *destination,
                                const double offsets[UNDERVOLT_DOMAIN_COUNT],
                                char *error,
                                size_t error_size) {
    if (source == NULL || destination == NULL || offsets == NULL) {
        set_error(error, error_size, "Parâmetros inválidos ao preparar o undervolt.");
        return -1;
    }
    if (undervolt_validate(offsets, error, error_size) != 0) {
        return -1;
    }

    FILE *input = fopen(source, "r");
    if (input == NULL && errno != ENOENT) {
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size, "Não foi possível abrir %s: %s", source, strerror(errno));
        }
        return -1;
    }

    FILE *output = fopen(destination, "w");
    if (output == NULL) {
        if (error != NULL && error_size > 0) {
            snprintf(error,
                     error_size,
                     "Não foi possível criar %s: %s",
                     destination,
                     strerror(errno));
        }
        if (input != NULL) {
            fclose(input);
        }
        return -1;
    }

    int result = 0;
    int written[UNDERVOLT_DOMAIN_COUNT] = {0};

    if (input != NULL) {
        char *line = NULL;
        size_t capacity = 0;
        ssize_t length = 0;

        while ((length = getline(&line, &capacity, input)) >= 0) {
            (void)length;
            int index = -1;
            int parsed = parse_domain_index(line, &index);

            if (parsed == 1) {
                if (write_domain_line(output, index, offsets[index]) != 0) {
                    result = -1;
                    break;
                }
                written[index] = 1;
            } else if (fputs(line, output) == EOF) {
                result = -1;
                break;
            }
        }
        free(line);
        fclose(input);
    } else {
        /*
         * intel-undervolt normally ships this file. The fallback below keeps
         * the generated configuration valid without inventing power or
         * temperature changes the user did not request.
         */
        if (fputs("# Generated by CPU Switch Control\n"
                  "enable no\n\n"
                  "# Voltage offsets in mV\n",
                  output) == EOF) {
            result = -1;
        }
    }

    if (result == 0) {
        for (int index = 0; index < UNDERVOLT_DOMAIN_COUNT; ++index) {
            if (!written[index] && write_domain_line(output, index, offsets[index]) != 0) {
                result = -1;
                break;
            }
        }
    }

    if (fclose(output) != 0) {
        result = -1;
    }

    if (result != 0) {
        set_error(error, error_size, "Falha ao gerar a configuração de undervolt.");
        return -1;
    }
    return 0;
}

size_t undervolt_parse_read_output(const char *output,
                                   double offsets[UNDERVOLT_DOMAIN_COUNT]) {
    if (output == NULL || offsets == NULL) {
        return 0;
    }

    size_t found = 0;
    for (int index = 0; index < UNDERVOLT_DOMAIN_COUNT; ++index) {
        char marker[16];
        snprintf(marker, sizeof(marker), "(%d):", index);

        const char *position = strstr(output, marker);
        if (position == NULL) {
            continue;
        }
        position += strlen(marker);

        char *end = NULL;
        errno = 0;
        double value = strtod(position, &end);
        if (position != end && errno == 0 && isfinite(value)) {
            offsets[index] = value;
            ++found;
        }
    }
    return found;
}
