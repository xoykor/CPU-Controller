/*
 * Testes da configuração de frequência.
 *
 * Estes testes verificam valores padrão, validação e o ciclo completo de
 * gravação + leitura do pequeno arquivo JSON usado pelo daemon.
 */

#define _POSIX_C_SOURCE 200809L

#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Basic round-trip tests keep the hand-written tiny JSON parser honest. */
int main(void) {
    CpuConfig config;
    cpu_config_defaults(&config);

    assert(config.low_threshold_pct == 6.0);
    assert(config.high_threshold_pct == 10.0);
    assert(config.low_frequency_khz == 1200000);
    assert(config.high_frequency_khz == 3500000);
    assert(config.interval_ms == 500);

    char error[256] = {0};
    assert(cpu_config_validate(&config, 800000, 4000000, 1, error, sizeof(error)) == 0);

    config.low_threshold_pct = 20.0;
    config.high_threshold_pct = 10.0;
    assert(cpu_config_validate(&config, 800000, 4000000, 1, error, sizeof(error)) != 0);

    char path[] = "/tmp/cpu-switch-config-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);

    cpu_config_defaults(&config);
    config.low_frequency_khz = 900000;
    config.high_frequency_khz = 3900000;
    assert(cpu_config_write(path, &config, error, sizeof(error)) == 0);

    CpuConfig loaded;
    assert(cpu_config_load(path, &loaded, error, sizeof(error)) == 0);
    assert(loaded.low_frequency_khz == 900000);
    assert(loaded.high_frequency_khz == 3900000);

    unlink(path);
    puts("config tests: ok");
    return 0;
}
