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
    undervolt_defaults(parsed);
    size_t found = undervolt_parse_read_output(
        "CPU (0): -70.31 mV\nGPU (1): 0.00 mV\nCPU Cache (2): -60.55 mV\n",
        parsed);
    assert(found == 3);
    assert(parsed[0] < -70.30 && parsed[0] > -70.32);
    assert(parsed[2] < -60.54 && parsed[2] > -60.56);

    unlink(source);
    unlink(destination);
    puts("undervolt tests: ok");
    return 0;
}
