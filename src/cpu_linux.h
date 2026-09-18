/*
 * CPU Switch Control - cpu_linux.h
 *
 * API pequena para consultar métricas da CPU e alterar limites de frequência no
 * Linux. A implementação fica isolada em cpu_linux.c para facilitar testes e
 * futuras adaptações para outros mecanismos do kernel.
 */

#ifndef CPU_SWITCH_CPU_LINUX_H
#define CPU_SWITCH_CPU_LINUX_H

#include <stddef.h>
#include <stdint.h>

/* Snapshot of the aggregate CPU counters exported by /proc/stat. */
typedef struct {
    uint64_t idle;
    uint64_t total;
} CpuTimes;

/* Read the aggregate CPU time counters from /proc/stat. */
int cpu_linux_read_times(CpuTimes *times, char *error, size_t error_size);

/* Convert two /proc/stat snapshots into a total CPU usage percentage. */
double cpu_linux_usage_percent(const CpuTimes *previous, const CpuTimes *current);

/*
 * Detect the frequency interval that is valid for every cpufreq policy.
 * POLICY_COUNT receives the number of usable policies found.
 */
int cpu_linux_detect_frequency_range(uint64_t *min_khz,
                                     uint64_t *max_khz,
                                     size_t *policy_count,
                                     char *error,
                                     size_t error_size);

/* Read the current frequency from the first usable cpufreq policy. */
int cpu_linux_read_current_frequency(uint64_t *frequency_khz,
                                     char *error,
                                     size_t error_size);

/*
 * Apply TARGET_KHZ to every cpufreq policy by setting min/max to the same
 * value. LOW_FREQUENCY_KHZ is used only to choose a write order that avoids
 * EINVAL on kernels that reject crossing min/max limits temporarily.
 */
int cpu_linux_set_all_policy_frequency(uint64_t target_khz,
                                       uint64_t low_frequency_khz,
                                       char *error,
                                       size_t error_size);

/* Return 1 when /proc/cpuinfo reports GenuineIntel, 0 otherwise. */
int cpu_linux_is_intel(void);

#endif
