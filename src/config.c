#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error(char *buffer, size_t size, const char *message) {
    if (buffer != NULL && size > 0) {
        snprintf(buffer, size, "%s", message);
    }
}

static char *read_file(const char *path, char *error, size_t error_size) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        if (errno == ENOENT) {
            return NULL;
        }
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size, "Não foi possível abrir %s: %s", path, strerror(errno));
        }
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        set_error(error, error_size, "Não foi possível medir o arquivo de configuração.");
        return NULL;
    }

    long length = ftell(file);
    if (length < 0 || length > 1024L * 1024L) {
        fclose(file);
        set_error(error, error_size, "Arquivo de configuração inválido ou grande demais.");
        return NULL;
    }
    rewind(file);

    char *contents = calloc((size_t)length + 1, 1);
    if (contents == NULL) {
        fclose(file);
        set_error(error, error_size, "Memória insuficiente para ler a configuração.");
        return NULL;
    }

    if (length > 0 && fread(contents, 1, (size_t)length, file) != (size_t)length) {
        free(contents);
        fclose(file);
        set_error(error, error_size, "Falha ao ler o arquivo de configuração.");
        return NULL;
    }

    fclose(file);
    return contents;
}

static const char *find_value(const char *json, const char *key) {
    char needle[96];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *position = strstr(json, needle);
    if (position == NULL) {
        return NULL;
    }
    position = strchr(position + strlen(needle), ':');
    return position == NULL ? NULL : position + 1;
}

static int parse_double(const char *json, const char *key, double *value) {
    const char *position = find_value(json, key);
    if (position == NULL) {
        return 0;
    }
    char *end = NULL;
    errno = 0;
    double parsed = strtod(position, &end);
    if (position == end || errno != 0) {
        return -1;
    }
    *value = parsed;
    return 1;
}

static int parse_u64(const char *json, const char *key, uint64_t *value) {
    const char *position = find_value(json, key);
    if (position == NULL) {
        return 0;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(position, &end, 10);
    if (position == end || errno != 0) {
        return -1;
    }
    *value = (uint64_t)parsed;
    return 1;
}

void cpu_config_defaults(CpuConfig *config) {
    if (config == NULL) {
        return;
    }
    config->low_threshold_pct = 6.0;
    config->high_threshold_pct = 10.0;
    config->low_frequency_khz = 1200000;
    config->high_frequency_khz = 3500000;
    config->interval_ms = 500;
}

int cpu_config_load(const char *path, CpuConfig *config, char *error, size_t error_size) {
    if (path == NULL || config == NULL) {
        set_error(error, error_size, "Parâmetros inválidos ao carregar configuração.");
        return -1;
    }

    cpu_config_defaults(config);
    errno = 0;
    char *json = read_file(path, error, error_size);
    if (json == NULL) {
        if (errno == ENOENT) {
            if (error != NULL && error_size > 0) {
                error[0] = '\0';
            }
            return 0;
        }
        return error != NULL && error[0] != '\0' ? -1 : 0;
    }

    int result = 0;
    if (parse_double(json, "low_threshold_pct", &config->low_threshold_pct) < 0 ||
        parse_double(json, "high_threshold_pct", &config->high_threshold_pct) < 0 ||
        parse_u64(json, "low_frequency_khz", &config->low_frequency_khz) < 0 ||
        parse_u64(json, "high_frequency_khz", &config->high_frequency_khz) < 0 ||
        parse_u64(json, "interval_ms", &config->interval_ms) < 0) {
        set_error(error, error_size, "Configuração JSON contém um valor numérico inválido.");
        result = -1;
    }

    free(json);
    return result;
}

int cpu_config_write(const char *path, const CpuConfig *config, char *error, size_t error_size) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size, "Não foi possível gravar %s: %s", path, strerror(errno));
        }
        return -1;
    }

    int written = fprintf(file,
                          "{\n"
                          "  \"low_threshold_pct\": %.1f,\n"
                          "  \"high_threshold_pct\": %.1f,\n"
                          "  \"low_frequency_khz\": %llu,\n"
                          "  \"high_frequency_khz\": %llu,\n"
                          "  \"interval_ms\": %llu\n"
                          "}\n",
                          config->low_threshold_pct,
                          config->high_threshold_pct,
                          (unsigned long long)config->low_frequency_khz,
                          (unsigned long long)config->high_frequency_khz,
                          (unsigned long long)config->interval_ms);

    if (written < 0 || fclose(file) != 0) {
        set_error(error, error_size, "Falha ao finalizar o arquivo de configuração.");
        return -1;
    }
    return 0;
}

int cpu_config_validate(const CpuConfig *config,
                        uint64_t min_khz,
                        uint64_t max_khz,
                        int check_range,
                        char *error,
                        size_t error_size) {
    if (config == NULL) {
        set_error(error, error_size, "Configuração ausente.");
        return -1;
    }
    if (config->low_threshold_pct < 0.0 || config->low_threshold_pct >= 100.0) {
        set_error(error, error_size, "O limite inferior deve estar entre 0 e 99%.");
        return -1;
    }
    if (config->high_threshold_pct <= 0.0 || config->high_threshold_pct > 100.0) {
        set_error(error, error_size, "O limite superior deve estar entre 1 e 100%.");
        return -1;
    }
    if (config->low_threshold_pct >= config->high_threshold_pct) {
        set_error(error, error_size, "O limite inferior precisa ser menor que o superior.");
        return -1;
    }
    if (config->low_frequency_khz > config->high_frequency_khz) {
        set_error(error, error_size, "A frequência baixa precisa ser menor ou igual à alta.");
        return -1;
    }
    if (config->interval_ms < 100 || config->interval_ms > 5000) {
        set_error(error, error_size, "O intervalo deve ficar entre 100 e 5000 ms.");
        return -1;
    }
    if (check_range && (config->low_frequency_khz < min_khz ||
                        config->low_frequency_khz > max_khz ||
                        config->high_frequency_khz < min_khz ||
                        config->high_frequency_khz > max_khz)) {
        set_error(error, error_size, "As frequências escolhidas estão fora da faixa detectada.");
        return -1;
    }
    return 0;
}
