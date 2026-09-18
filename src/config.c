/*
 * CPU Switch Control - config.c
 *
 * Leitura, gravação e validação da configuração do daemon de frequência.
 * O formato JSON é propositalmente pequeno; o parser lê apenas as chaves que o
 * programa conhece, mantendo o código sem dependência de uma biblioteca JSON.
 */

#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Small error-reporting helpers                                              */
/* ------------------------------------------------------------------------- */

/* Copia uma mensagem de erro para o buffer do chamador quando ele foi fornecido. */
static void set_error(char *buffer, size_t size, const char *message) {
    if (buffer != NULL && size > 0) {
        snprintf(buffer, size, "%s", message);
    }
}

/* Formata erros de sistema preservando o contexto da operação e a mensagem de errno. */
static void set_errno_error(char *buffer,
                            size_t size,
                            const char *prefix,
                            const char *path) {
    if (buffer != NULL && size > 0) {
        snprintf(buffer, size, "%s %s: %s", prefix, path, strerror(errno));
    }
}

/* ------------------------------------------------------------------------- */
/* Minimal JSON reader                                                        */
/* ------------------------------------------------------------------------- */

/*
 * The configuration schema contains only five numeric fields. Pulling in a
 * full JSON library would add a dependency for a format this small, so the
 * reader below intentionally looks up only the exact keys this program owns.
 * Unknown JSON keys are ignored, which also gives us forward compatibility.
 */
static char *read_entire_file(const char *path,
                              int *missing,
                              char *error,
                              size_t error_size) {
    *missing = 0;

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        if (errno == ENOENT) {
            *missing = 1;
            return NULL;
        }
        set_errno_error(error, error_size, "Não foi possível abrir", path);
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        set_error(error, error_size, "Não foi possível medir o arquivo de configuração.");
        fclose(file);
        return NULL;
    }

    long length = ftell(file);
    if (length < 0 || length > 1024L * 1024L) {
        set_error(error, error_size, "Arquivo de configuração inválido ou grande demais.");
        fclose(file);
        return NULL;
    }
    rewind(file);

    char *contents = calloc((size_t)length + 1U, 1U);
    if (contents == NULL) {
        set_error(error, error_size, "Memória insuficiente para ler a configuração.");
        fclose(file);
        return NULL;
    }

    if (length > 0 && fread(contents, 1U, (size_t)length, file) != (size_t)length) {
        set_error(error, error_size, "Falha ao ler o arquivo de configuração.");
        free(contents);
        fclose(file);
        return NULL;
    }

    fclose(file);
    return contents;
}

/* Return a pointer to the text immediately after the ':' of KEY. */
static const char *find_json_value(const char *json, const char *key) {
    char needle[96];
    snprintf(needle, sizeof(needle), "\"%s\"", key);

    const char *position = strstr(json, needle);
    if (position == NULL) {
        return NULL;
    }

    position = strchr(position + strlen(needle), ':');
    return position == NULL ? NULL : position + 1;
}

/* Localiza uma chave numérica no JSON simples e converte seu valor para double. */
static int parse_double_field(const char *json, const char *key, double *value) {
    const char *position = find_json_value(json, key);
    if (position == NULL) {
        return 0; /* Missing fields deliberately keep their defaults. */
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

/* Lê um inteiro sem sinal do JSON, usado para frequências e intervalos em milissegundos. */
static int parse_u64_field(const char *json, const char *key, uint64_t *value) {
    const char *position = find_json_value(json, key);
    if (position == NULL) {
        return 0;
    }

    while (isspace((unsigned char)*position)) {
        ++position;
    }
    if (*position == '-') {
        return -1;
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

/* ------------------------------------------------------------------------- */
/* Public configuration API                                                   */
/* ------------------------------------------------------------------------- */

/* Define valores conservadores usados quando ainda não existe arquivo de configuração válido. */
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

/* Carrega apenas as chaves conhecidas; campos ausentes continuam com seus valores padrão. */
int cpu_config_load(const char *path,
                    CpuConfig *config,
                    char *error,
                    size_t error_size) {
    if (path == NULL || config == NULL) {
        set_error(error, error_size, "Parâmetros inválidos ao carregar configuração.");
        return -1;
    }

    cpu_config_defaults(config);
    if (error != NULL && error_size > 0) {
        error[0] = '\0';
    }

    int missing = 0;
    char *json = read_entire_file(path, &missing, error, error_size);
    if (json == NULL) {
        return missing ? 0 : -1;
    }

    int invalid = 0;
    invalid |= parse_double_field(json, "low_threshold_pct", &config->low_threshold_pct) < 0;
    invalid |= parse_double_field(json, "high_threshold_pct", &config->high_threshold_pct) < 0;
    invalid |= parse_u64_field(json, "low_frequency_khz", &config->low_frequency_khz) < 0;
    invalid |= parse_u64_field(json, "high_frequency_khz", &config->high_frequency_khz) < 0;
    invalid |= parse_u64_field(json, "interval_ms", &config->interval_ms) < 0;

    free(json);

    if (invalid) {
        set_error(error, error_size, "Configuração JSON contém um valor numérico inválido.");
        return -1;
    }
    return 0;
}

/* Serializa a configuração em JSON legível para humanos e estável entre versões. */
int cpu_config_write(const char *path,
                     const CpuConfig *config,
                     char *error,
                     size_t error_size) {
    if (path == NULL || config == NULL) {
        set_error(error, error_size, "Parâmetros inválidos ao salvar configuração.");
        return -1;
    }

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        set_errno_error(error, error_size, "Não foi possível gravar", path);
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

/* Centraliza todas as regras que precisam valer antes de salvar ou iniciar o daemon. */
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

    if (check_range &&
        (config->low_frequency_khz < min_khz ||
         config->low_frequency_khz > max_khz ||
         config->high_frequency_khz < min_khz ||
         config->high_frequency_khz > max_khz)) {
        set_error(error, error_size, "As frequências escolhidas estão fora da faixa detectada.");
        return -1;
    }

    return 0;
}
