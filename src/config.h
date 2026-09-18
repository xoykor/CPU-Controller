/*
 * CPU Switch Control - config.h
 *
 * Estruturas e funções compartilhadas pela GUI, pelos testes e pelo daemon para
 * representar a configuração de frequência de maneira única.
 */

#ifndef CPU_SWITCH_CONFIG_H
#define CPU_SWITCH_CONFIG_H

#include <stddef.h>
#include <stdint.h>

/*
 * Shared configuration used by both the graphical application and the daemon.
 *
 * Keeping this structure small and explicit makes the on-disk JSON format easy
 * to inspect by hand and preserves compatibility with the previous Rust/Python
 * version of CPU Switch Control.
 */
typedef struct {
    double low_threshold_pct;
    double high_threshold_pct;
    uint64_t low_frequency_khz;
    uint64_t high_frequency_khz;
    uint64_t interval_ms;
} CpuConfig;

/* Fill a configuration structure with safe, conservative defaults. */
void cpu_config_defaults(CpuConfig *config);

/*
 * Load the known numeric keys from PATH.
 * Missing keys keep their default values; a missing file is not an error.
 */
int cpu_config_load(const char *path, CpuConfig *config, char *error, size_t error_size);

/* Write the canonical JSON representation used by the project. */
int cpu_config_write(const char *path,
                     const CpuConfig *config,
                     char *error,
                     size_t error_size);

/*
 * Validate logical relationships and, when CHECK_RANGE is non-zero, verify
 * that both selected frequencies fit inside the detected hardware range.
 */
int cpu_config_validate(const CpuConfig *config,
                        uint64_t min_khz,
                        uint64_t max_khz,
                        int check_range,
                        char *error,
                        size_t error_size);

#endif
