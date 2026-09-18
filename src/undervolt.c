/*
 * CPU Switch Control - undervolt.c
 *
 * Manipula os offsets de tensão usados pelo backend intel-undervolt.
 * O módulo sabe interpretar a saída do backend, preservar o arquivo de
 * configuração existente e alterar somente os domínios detectados no hardware.
 */

#define _POSIX_C_SOURCE 200809L

#include "undervolt.h"
#include "numeric_ascii.h"

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

/* Helper local para mensagens de erro da camada de undervolt. */
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
/* Extrai somente o índice da linha `undervolt N ...`; o nome textual pode variar entre CPUs. */
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
/* Lê o último token numérico de uma linha, que no formato do backend é o offset em mV. */
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
    double parsed = numeric_ascii_strtod(number, &number_end);
    if (number == number_end || *number_end != '\0' || errno != 0 || !isfinite(parsed)) {
        return -1;
    }

    *value = parsed;
    return 0;
}

/*
 * Escreve uma linha de domínio usando índice estável e ponto decimal.
 *
 * A GUI pode exibir "-60,00" em pt_BR, mas intel-undervolt exige "-60.00".
 * Por isso a conversão para texto nunca usa diretamente a locale da interface.
 */
static int write_domain_line(FILE *file, int index, double offset_mv) {
    char formatted_offset[64];
    if (numeric_ascii_format_double(formatted_offset,
                                    sizeof(formatted_offset),
                                    2,
                                    offset_mv) < 0) {
        return -1;
    }

    return fprintf(file,
                   "undervolt %d '%s' %s\n",
                   index,
                   UNDERVOLT_DOMAIN_NAMES[index],
                   formatted_offset) >= 0
               ? 0
               : -1;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                 */
/* ------------------------------------------------------------------------- */

/* Inicializa todos os domínios em 0 mV, isto é, sem offset. */
void undervolt_defaults(double offsets[UNDERVOLT_DOMAIN_COUNT]) {
    if (offsets == NULL) {
        return;
    }
    for (size_t index = 0; index < UNDERVOLT_DOMAIN_COUNT; ++index) {
        offsets[index] = 0.0;
    }
}

/* Impede overvolt pela aplicação e rejeita valores fora do intervalo suportado pela GUI. */
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

/* Lê os offsets existentes sem alterar outras opções do arquivo intel-undervolt.conf. */
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

/* Decide se um domínio deve ser alterado; máscara NULL significa comportamento legado: todos. */
static int domain_selected(
    int index,
    const unsigned char present[UNDERVOLT_DOMAIN_COUNT]) {
    return present == NULL || present[index] != 0;
}

/* Cria uma cópia editada preservando linhas e domínios que não foram detectados no hardware. */
int undervolt_write_config_copy_masked(
    const char *source,
    const char *destination,
    const double offsets[UNDERVOLT_DOMAIN_COUNT],
    const unsigned char present[UNDERVOLT_DOMAIN_COUNT],
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

            if (parsed == 1 && domain_selected(index, present)) {
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
        if (fputs("# Generated by CPU Switch Control\n"
                  "enable no\n\n"
                  "# Voltage offsets in mV\n",
                  output) == EOF) {
            result = -1;
        }
    }

    if (result == 0) {
        for (int index = 0; index < UNDERVOLT_DOMAIN_COUNT; ++index) {
            if (domain_selected(index, present) &&
                !written[index] &&
                write_domain_line(output, index, offsets[index]) != 0) {
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

/* Wrapper compatível que reescreve todos os domínios quando nenhuma máscara é necessária. */
int undervolt_write_config_copy(const char *source,
                                const char *destination,
                                const double offsets[UNDERVOLT_DOMAIN_COUNT],
                                char *error,
                                size_t error_size) {
    return undervolt_write_config_copy_masked(source,
                                              destination,
                                              offsets,
                                              NULL,
                                              error,
                                              error_size);
}

/* Interpreta `intel-undervolt read` e produz simultaneamente valores e máscara de domínios presentes. */
size_t undervolt_parse_read_output_masked(
    const char *output,
    double offsets[UNDERVOLT_DOMAIN_COUNT],
    unsigned char present[UNDERVOLT_DOMAIN_COUNT]) {
    if (output == NULL || offsets == NULL) {
        return 0;
    }

    if (present != NULL) {
        memset(present, 0, UNDERVOLT_DOMAIN_COUNT * sizeof(present[0]));
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
        double value = numeric_ascii_strtod(position, &end);
        if (position != end && errno == 0 && isfinite(value)) {
            offsets[index] = value;
            if (present != NULL) {
                present[index] = 1;
            }
            ++found;
        }
    }
    return found;
}

/* Wrapper simples para chamadores interessados apenas nos valores. */
size_t undervolt_parse_read_output(const char *output,
                                   double offsets[UNDERVOLT_DOMAIN_COUNT]) {
    return undervolt_parse_read_output_masked(output, offsets, NULL);
}
