/*
 * Testes da configuração de frequência.
 *
 * Estes testes verificam valores padrão, validação e o ciclo completo de
 * gravação + leitura do pequeno arquivo JSON usado pelo daemon.
 */

#define _POSIX_C_SOURCE 200809L

#include "config.h"

#include <assert.h>
#include <locale.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Helper pequeno para conferir o texto JSON realmente gravado no disco. */
static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    assert(length >= 0);
    rewind(file);

    char *contents = calloc((size_t)length + 1U, 1U);
    assert(contents != NULL);
    assert(fread(contents, 1U, (size_t)length, file) == (size_t)length);
    fclose(file);
    return contents;
}

/* Basic round-trip tests keep the hand-written tiny JSON parser honest. */
int main(void) {
    /*
     * Usa a locale real do processo. O CI executa também em pt_BR.UTF-8 para
     * garantir que o arquivo continue usando ponto decimal.
     */
    assert(setlocale(LC_ALL, "") != NULL);
    /* 1) Os padrões são o contrato usado quando ainda não existe configuração. */
    CpuConfig config;
    cpu_config_defaults(&config);

    assert(config.low_threshold_pct == 6.0);
    assert(config.high_threshold_pct == 10.0);
    assert(config.low_frequency_khz == 1200000);
    assert(config.high_frequency_khz == 3500000);
    assert(config.interval_ms == 500);

    /* 2) Uma configuração padrão deve passar pela mesma validação da GUI. */
    char error[256] = {0};
    assert(cpu_config_validate(&config, 800000, 4000000, 1, error, sizeof(error)) == 0);

    /* 3) A histerese inválida (limite baixo >= alto) precisa ser rejeitada. */
    config.low_threshold_pct = 20.0;
    config.high_threshold_pct = 10.0;
    assert(cpu_config_validate(&config, 800000, 4000000, 1, error, sizeof(error)) != 0);

    char path[] = "/tmp/cpu-switch-config-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);

    cpu_config_defaults(&config);
    config.low_threshold_pct = 6.5;
    config.high_threshold_pct = 10.5;
    config.low_frequency_khz = 900000;
    config.high_frequency_khz = 3900000;
    assert(cpu_config_write(path, &config, error, sizeof(error)) == 0);

    /* JSON é formato de máquina: ponto é obrigatório mesmo com GUI em pt_BR. */
    char *written_json = read_file(path);
    assert(strstr(written_json, "\"low_threshold_pct\": 6.5") != NULL);
    assert(strstr(written_json, "\"high_threshold_pct\": 10.5") != NULL);
    assert(strstr(written_json, "6,5") == NULL);
    assert(strstr(written_json, "10,5") == NULL);
    free(written_json);

    CpuConfig loaded;
    assert(cpu_config_load(path, &loaded, error, sizeof(error)) == 0);
    assert(loaded.low_threshold_pct == 6.5);
    assert(loaded.high_threshold_pct == 10.5);
    assert(loaded.low_frequency_khz == 900000);
    assert(loaded.high_frequency_khz == 3900000);

    unlink(path);
    puts("config tests: ok");
    return 0;
}
