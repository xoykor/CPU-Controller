#ifndef CPU_SWITCH_CONFIG_H
#define CPU_SWITCH_CONFIG_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    double low_threshold_pct;
    double high_threshold_pct;
    uint64_t low_frequency_khz;
    uint64_t high_frequency_khz;
    uint64_t interval_ms;
} CpuConfig;

void cpu_config_defaults(CpuConfig *config);
int cpu_config_load(const char *path, CpuConfig *config, char *error, size_t error_size);
int cpu_config_write(const char *path, const CpuConfig *config, char *error, size_t error_size);
int cpu_config_validate(const CpuConfig *config,
                        uint64_t min_khz,
                        uint64_t max_khz,
                        int check_range,
                        char *error,
                        size_t error_size);

#endif
