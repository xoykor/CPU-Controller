#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "cpu_linux.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define DEFAULT_CONFIG_PATH "/etc/cpu-clock-switch.json"

/*
 * The daemon is intentionally small: it samples total CPU usage, applies the
 * low/high threshold hysteresis, and asks cpu_linux.c to write cpufreq limits.
 * All GUI concerns and all undervolt logic live elsewhere.
 */
static volatile sig_atomic_t running = 1;

static void handle_stop_signal(int signal_number) {
    (void)signal_number;
    running = 0;
}

static void sleep_ms(uint64_t milliseconds) {
    struct timespec remaining = {
        .tv_sec = (time_t)(milliseconds / 1000U),
        .tv_nsec = (long)((milliseconds % 1000U) * 1000000ULL),
    };

    while (running && nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {
        /* Resume the remaining part of the sleep after an unrelated signal. */
    }
}

static CpuConfig load_runtime_config(const char *path) {
    CpuConfig config;
    char error[256] = {0};

    if (cpu_config_load(path, &config, error, sizeof(error)) != 0) {
        fprintf(stderr,
                "cpu-clock-switch: %s; usando configuração padrão\n",
                error[0] != '\0' ? error : "falha ao ler configuração");
        cpu_config_defaults(&config);
    }

    if (cpu_config_validate(&config, 0, 0, 0, error, sizeof(error)) != 0) {
        fprintf(stderr,
                "cpu-clock-switch: configuração inválida (%s); usando padrões\n",
                error);
        cpu_config_defaults(&config);
    }

    return config;
}

int main(void) {
    const char *config_path = getenv("CPU_CLOCK_SWITCH_CONFIG");
    if (config_path == NULL || *config_path == '\0') {
        config_path = DEFAULT_CONFIG_PATH;
    }

    CpuConfig config = load_runtime_config(config_path);

    /* Fail early instead of keeping a useless systemd service alive. */
    uint64_t hardware_min = 0;
    uint64_t hardware_max = 0;
    size_t policy_count = 0;
    char error[256] = {0};
    if (cpu_linux_detect_frequency_range(&hardware_min,
                                         &hardware_max,
                                         &policy_count,
                                         error,
                                         sizeof(error)) != 0) {
        fprintf(stderr, "cpu-clock-switch: %s\n", error);
        return 1;
    }

    (void)hardware_min;
    (void)hardware_max;
    fprintf(stdout,
            "cpu-clock-switch: controlando %zu política(s) cpufreq\n",
            policy_count);

    struct sigaction action = {0};
    action.sa_handler = handle_stop_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    CpuTimes previous;
    if (cpu_linux_read_times(&previous, error, sizeof(error)) != 0) {
        fprintf(stderr, "cpu-clock-switch: %s\n", error);
        return 1;
    }

    enum {
        STATE_UNKNOWN,
        STATE_LOW,
        STATE_HIGH,
    } state = STATE_UNKNOWN;

    while (running) {
        sleep_ms(config.interval_ms);
        if (!running) {
            break;
        }

        CpuTimes current;
        if (cpu_linux_read_times(&current, error, sizeof(error)) != 0) {
            fprintf(stderr, "cpu-clock-switch: %s\n", error);
            continue;
        }

        double usage = cpu_linux_usage_percent(&previous, &current);
        previous = current;

        /*
         * Hysteresis is the key behavior of this service:
         *   usage < low  -> low clock
         *   usage > high -> high clock
         *   between them -> preserve the previous state
         *
         * The middle band prevents rapid low/high oscillation around one
         * threshold when CPU load is noisy.
         */
        if (usage < config.low_threshold_pct && state != STATE_LOW) {
            if (cpu_linux_set_all_policy_frequency(config.low_frequency_khz,
                                                   config.low_frequency_khz,
                                                   error,
                                                   sizeof(error)) == 0) {
                state = STATE_LOW;
            } else {
                fprintf(stderr, "cpu-clock-switch: %s\n", error);
            }
        } else if (usage > config.high_threshold_pct && state != STATE_HIGH) {
            if (cpu_linux_set_all_policy_frequency(config.high_frequency_khz,
                                                   config.low_frequency_khz,
                                                   error,
                                                   sizeof(error)) == 0) {
                state = STATE_HIGH;
            } else {
                fprintf(stderr, "cpu-clock-switch: %s\n", error);
            }
        }
    }

    return 0;
}
