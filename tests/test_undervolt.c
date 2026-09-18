#define _POSIX_C_SOURCE 200809L

#include "undervolt.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

int main(void) {
    double offsets[UNDERVOLT_DOMAIN_COUNT] = {-70.0, 0.0, -60.5, 0.0, 0.0};
    char error[256] = {0};
    assert(undervolt_validate(offsets, error, sizeof(error)) == 0);

    offsets[0] = 10.0;
    assert(undervolt_validate(offsets, error, sizeof(error)) != 0);
    offsets[0] = -70.0;

    char source[] = "/tmp/intel-undervolt-source-XXXXXX";
    int source_fd = mkstemp(source);
    assert(source_fd >= 0);
    FILE *source_file = fdopen(source_fd, "w");
    assert(source_file != NULL);
    fputs("enable no\n"
          "# keep this comment\n"
          "undervolt 0 'CPU' -10\n"
          "undervolt 2 'CPU Cache' -20\n"
          "interval 5000\n",
          source_file);
    fclose(source_file);

    char destination[] = "/tmp/intel-undervolt-dest-XXXXXX";
    int destination_fd = mkstemp(destination);
    assert(destination_fd >= 0);
    close(destination_fd);

    assert(undervolt_write_config_copy(source,
                                       destination,
                                       offsets,
                                       error,
                                       sizeof(error)) == 0);

    char *rewritten = read_file(destination);
    assert(strstr(rewritten, "# keep this comment") != NULL);
    assert(strstr(rewritten, "interval 5000") != NULL);
    assert(strstr(rewritten, "undervolt 0 'CPU' -70.00") != NULL);
    assert(strstr(rewritten, "undervolt 2 'CPU Cache' -60.50") != NULL);
    free(rewritten);

    double parsed[UNDERVOLT_DOMAIN_COUNT];
    unsigned char present[UNDERVOLT_DOMAIN_COUNT];
    undervolt_defaults(parsed);
    size_t found = undervolt_parse_read_output_masked(
        "CPU (0): -70.31 mV\nCACHE (2): -60.55 mV\nUncore (3): -25.00 mV\n",
        parsed,
        present);
    assert(found == 3);
    assert(present[0] == 1);
    assert(present[1] == 0);
    assert(present[2] == 1);
    assert(present[3] == 1);
    assert(present[4] == 0);
    assert(parsed[0] < -70.30 && parsed[0] > -70.32);
    assert(parsed[2] < -60.54 && parsed[2] > -60.56);
    assert(parsed[3] < -24.99 && parsed[3] > -25.01);

    char masked_source[] = "/tmp/intel-undervolt-masked-source-XXXXXX";
    int masked_source_fd = mkstemp(masked_source);
    assert(masked_source_fd >= 0);
    FILE *masked_source_file = fdopen(masked_source_fd, "w");
    assert(masked_source_file != NULL);
    fputs("undervolt 0 'CPU' -10\n"
          "undervolt 1 'GPU' -5\n"
          "undervolt 2 'CPU Cache' -20\n"
          "undervolt 3 'Uncore' -15\n"
          "undervolt 4 'Analog I/O' -7\n",
          masked_source_file);
    fclose(masked_source_file);

    char masked_destination[] = "/tmp/intel-undervolt-masked-dest-XXXXXX";
    int masked_destination_fd = mkstemp(masked_destination);
    assert(masked_destination_fd >= 0);
    close(masked_destination_fd);

    double masked_offsets[UNDERVOLT_DOMAIN_COUNT] = {-70.0, 0.0, -60.5, -25.0, 0.0};
    unsigned char masked_present[UNDERVOLT_DOMAIN_COUNT] = {1, 0, 1, 1, 0};
    assert(undervolt_write_config_copy_masked(masked_source,
                                              masked_destination,
                                              masked_offsets,
                                              masked_present,
                                              error,
                                              sizeof(error)) == 0);

    char *masked_rewritten = read_file(masked_destination);
    assert(strstr(masked_rewritten, "undervolt 0 'CPU' -70.00") != NULL);
    assert(strstr(masked_rewritten, "undervolt 1 'GPU' -5") != NULL);
    assert(strstr(masked_rewritten, "undervolt 2 'CPU Cache' -60.50") != NULL);
    assert(strstr(masked_rewritten, "undervolt 3 'Uncore' -25.00") != NULL);
    assert(strstr(masked_rewritten, "undervolt 4 'Analog I/O' -7") != NULL);
    free(masked_rewritten);

    unlink(masked_source);
    unlink(masked_destination);

    unlink(source);
    unlink(destination);
    puts("undervolt tests: ok");
    return 0;
}
