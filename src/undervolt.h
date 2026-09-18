#ifndef CPU_SWITCH_UNDERVOLT_H
#define CPU_SWITCH_UNDERVOLT_H

#include <stddef.h>

#define UNDERVOLT_DOMAIN_COUNT 5
#define UNDERVOLT_MIN_MV (-150.0)
#define UNDERVOLT_MAX_MV (0.0)

extern const char *UNDERVOLT_DOMAIN_NAMES[UNDERVOLT_DOMAIN_COUNT];

/* Fill every voltage domain with 0 mV (no offset). */
void undervolt_defaults(double offsets[UNDERVOLT_DOMAIN_COUNT]);

/*
 * Read the five `undervolt` entries from intel-undervolt.conf.
 * Missing entries keep their default 0 mV value; a missing file is accepted.
 */
int undervolt_load_config(const char *path,
                          double offsets[UNDERVOLT_DOMAIN_COUNT],
                          char *error,
                          size_t error_size);

/*
 * Create DESTINATION from SOURCE while preserving comments and unrelated
 * intel-undervolt settings. Only the five voltage-domain lines are replaced.
 * If SOURCE does not exist, a small valid configuration is generated.
 */
int undervolt_write_config_copy(const char *source,
                                const char *destination,
                                const double offsets[UNDERVOLT_DOMAIN_COUNT],
                                char *error,
                                size_t error_size);

/*
 * Same operation, but only domains marked as present are changed/created.
 * Unmarked domains already present in SOURCE are preserved byte-for-byte.
 */
int undervolt_write_config_copy_masked(
    const char *source,
    const char *destination,
    const double offsets[UNDERVOLT_DOMAIN_COUNT],
    const unsigned char present[UNDERVOLT_DOMAIN_COUNT],
    char *error,
    size_t error_size);

/* Reject positive offsets and values outside the UI's supported range. */
int undervolt_validate(const double offsets[UNDERVOLT_DOMAIN_COUNT],
                       char *error,
                       size_t error_size);

/*
 * Parse output produced by `intel-undervolt read`, for example:
 *     CPU (0): -70.31 mV
 * Returns how many of the five domains were found.
 */
size_t undervolt_parse_read_output(const char *output,
                                   double offsets[UNDERVOLT_DOMAIN_COUNT]);

/*
 * Parse hardware output and additionally mark exactly which voltage domains
 * intel-undervolt reported. The present array is cleared before parsing.
 */
size_t undervolt_parse_read_output_masked(
    const char *output,
    double offsets[UNDERVOLT_DOMAIN_COUNT],
    unsigned char present[UNDERVOLT_DOMAIN_COUNT]);

#endif
