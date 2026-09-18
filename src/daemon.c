#define _POSIX_C_SOURCE 200809L

#include "config.h"

#include <errno.h>
#include <glob.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_CONFIG_PATH "/etc/cpu-clock-switch.json"

typedef struct {
    uint64_t idle;
    uint64_t total;
} CpuTimes;

static volatile sig_atomic_t running = 1;

static void handle_signal(int signal_number) {
    (void)signal_number;
    running = 0;
}

static int read_u64_file(const char *path, uint64_t *value) {
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }
    unsigned long long parsed = 0;
    int ok = fscanf(file, "%llu", &parsed) == 1 ? 0 : -1;
    fclose(file);
    if (ok == 0) {
        *value = (uint64_t)parsed;
    }
    return ok;
}

static void write_u64_file(const char *path, uint64_t value) {
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        return;
    }
    fprintf(file, "%llu", (unsigned long long)value);
    fclose(file);
}

static int read_cpu_times(CpuTimes *times) {
    FILE *file = fopen("/proc/stat", "r");
    if (file == NULL) {
        return -1;
    }

    char line[1024];
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return -1;
    }
    fclose(file);

    unsigned long long user = 0, nice = 0, system = 0, idle = 0;
    unsigned long long iowait = 0, irq = 0, softirq = 0, steal = 0;
    int count = sscanf(line,
                       "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                       &user,
                       &nice,
                       &system,
                       &idle,
                       &iowait,
                       &irq,
                       &softirq,
                       &steal);
    if (count < 4) {
        return -1;
    }

    times->idle = idle + (count >= 5 ? iowait : 0);
    times->total = user + nice + system + idle + iowait + irq + softirq + steal;
    return 0;
}

static void set_policy_frequency(const char *policy, uint64_t target, uint64_t low_frequency) {
    char path[512];
    uint64_t hardware_min = 0;
    uint64_t hardware_max = 0;

    snprintf(path, sizeof(path), "%s/cpuinfo_min_freq", policy);
    if (read_u64_file(path, &hardware_min) != 0) {
        return;
    }
    snprintf(path, sizeof(path), "%s/cpuinfo_max_freq", policy);
    if (read_u64_file(path, &hardware_max) != 0 || hardware_min > hardware_max) {
        return;
    }

    uint64_t frequency = target;
    if (frequency < hardware_min) {
        frequency = hardware_min;
    }
    if (frequency > hardware_max) {
        frequency = hardware_max;
    }

    char min_path[512];
    char max_path[512];
    snprintf(min_path, sizeof(min_path), "%s/scaling_min_freq", policy);
    snprintf(max_path, sizeof(max_path), "%s/scaling_max_freq", policy);

    if (target <= low_frequency) {
        write_u64_file(min_path, frequency);
        write_u64_file(max_path, frequency);
    } else {
        write_u64_file(max_path, frequency);
        write_u64_file(min_path, frequency);
    }
}

static void set_frequency(const glob_t *policies, uint64_t target, uint64_t low_frequency) {
    for (size_t index = 0; index < policies->gl_pathc; ++index) {
        set_policy_frequency(policies->gl_pathv[index], target, low_frequency);
    }
}

static void sleep_ms(uint64_t milliseconds) {
    struct timespec request = {
        .tv_sec = (time_t)(milliseconds / 1000),
        .tv_nsec = (long)((milliseconds % 1000) * 1000000ULL),
    };
    while (running && nanosleep(&request, &request) != 0 && errno == EINTR) {
    }
}

int main(void) {
    const char *config_path = getenv("CPU_CLOCK_SWITCH_CONFIG");
    if (config_path == NULL || *config_path == '\0') {
        config_path = DEFAULT_CONFIG_PATH;
    }

    CpuConfig config;
    char error[256] = {0};
    if (cpu_config_load(config_path, &config, error, sizeof(error)) != 0) {
        fprintf(stderr, "cpu-clock-switch: %s\n", error);
        cpu_config_defaults(&config);
    }
    if (cpu_config_validate(&config, 0, 0, 0, error, sizeof(error)) != 0) {
        fprintf(stderr, "cpu-clock-switch: configuração inválida (%s); usando padrões\n", error);
        cpu_config_defaults(&config);
    }

    glob_t policies = {0};
    if (glob("/sys/devices/system/cpu/cpufreq/policy*", 0, NULL, &policies) != 0 ||
        policies.gl_pathc == 0) {
        fprintf(stderr, "cpu-clock-switch: nenhuma política cpufreq encontrada\n");
        globfree(&policies);
        return 1;
    }

    struct sigaction action = {0};
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    CpuTimes previous;
    if (read_cpu_times(&previous) != 0) {
        fprintf(stderr, "cpu-clock-switch: não foi possível ler /proc/stat\n");
        globfree(&policies);
        return 1;
    }

    enum { STATE_NONE, STATE_LOW, STATE_HIGH } state = STATE_NONE;

    while (running) {
        sleep_ms(config.interval_ms);
        if (!running) {
            break;
        }

        CpuTimes current;
        if (read_cpu_times(&current) != 0) {
            continue;
        }

        uint64_t total_delta = current.total - previous.total;
        uint64_t idle_delta = current.idle - previous.idle;
        previous = current;
        if (total_delta == 0) {
            continue;
        }

        double usage = 100.0 * (1.0 - (double)idle_delta / (double)total_delta);
        if (usage < config.low_threshold_pct && state != STATE_LOW) {
            set_frequency(&policies, config.low_frequency_khz, config.low_frequency_khz);
            state = STATE_LOW;
        } else if (usage > config.high_threshold_pct && state != STATE_HIGH) {
            set_frequency(&policies, config.high_frequency_khz, config.low_frequency_khz);
            state = STATE_HIGH;
        }
    }

    globfree(&policies);
    return 0;
}
