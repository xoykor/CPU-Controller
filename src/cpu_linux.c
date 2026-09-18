/*
 * CPU Switch Control - cpu_linux.c
 *
 * Integração direta com interfaces do kernel Linux: /proc/stat para uso de CPU,
 * sysfs/cpufreq para frequências e /proc/cpuinfo para identificação do fabricante.
 * Nenhuma decisão de interface gráfica fica neste módulo.
 */

#define _POSIX_C_SOURCE 200809L

#include "cpu_linux.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CPUFREQ_PATH "/sys/devices/system/cpu/cpufreq"

/* ------------------------------------------------------------------------- */
/* Generic file helpers                                                       */
/* ------------------------------------------------------------------------- */

/* Helper local para devolver mensagens de erro sem depender da interface gráfica. */
static void set_error(char *buffer, size_t size, const char *message) {
    if (buffer != NULL && size > 0) {
        snprintf(buffer, size, "%s", message);
    }
}

/* Lê um único inteiro de um arquivo sysfs, padrão usado por quase todos os atributos cpufreq. */
static int read_u64_file(const char *path, uint64_t *value) {
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }

    unsigned long long parsed = 0;
    int result = fscanf(file, "%llu", &parsed) == 1 ? 0 : -1;
    fclose(file);

    if (result == 0) {
        *value = (uint64_t)parsed;
    }
    return result;
}

/* Escreve um inteiro em sysfs; o daemon já roda com privilégios suficientes para isso. */
static int write_u64_file(const char *path, uint64_t value) {
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        return -1;
    }

    int result = fprintf(file, "%llu", (unsigned long long)value) >= 0 ? 0 : -1;
    if (fclose(file) != 0) {
        result = -1;
    }
    return result;
}

/* cpufreq directories we care about are named policy0, policy1, ... */
/* Filtra somente diretórios policyN dentro de /sys/devices/system/cpu/cpufreq. */
static int is_policy_name(const char *name) {
    if (strncmp(name, "policy", 6) != 0 || name[6] == '\0') {
        return 0;
    }

    for (const char *cursor = name + 6; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') {
            return 0;
        }
    }
    return 1;
}

/* Monta com segurança o caminho de um atributo pertencente a uma política cpufreq. */
static int build_policy_file_path(char *buffer,
                                  size_t buffer_size,
                                  const char *policy_name,
                                  const char *file_name) {
    int written = snprintf(buffer,
                           buffer_size,
                           "%s/%s/%s",
                           CPUFREQ_PATH,
                           policy_name,
                           file_name);
    return written >= 0 && (size_t)written < buffer_size ? 0 : -1;
}

/* ------------------------------------------------------------------------- */
/* CPU usage                                                                  */
/* ------------------------------------------------------------------------- */

/* Lê a primeira linha de /proc/stat e soma os contadores necessários para calcular uso total. */
int cpu_linux_read_times(CpuTimes *times, char *error, size_t error_size) {
    if (times == NULL) {
        set_error(error, error_size, "Destino inválido para leitura de uso da CPU.");
        return -1;
    }

    FILE *file = fopen("/proc/stat", "r");
    if (file == NULL) {
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size, "Não foi possível abrir /proc/stat: %s", strerror(errno));
        }
        return -1;
    }

    char line[1024];
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        set_error(error, error_size, "Não foi possível ler a linha global de /proc/stat.");
        return -1;
    }
    fclose(file);

    unsigned long long user = 0;
    unsigned long long nice = 0;
    unsigned long long system = 0;
    unsigned long long idle = 0;
    unsigned long long iowait = 0;
    unsigned long long irq = 0;
    unsigned long long softirq = 0;
    unsigned long long steal = 0;

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
        set_error(error, error_size, "Formato inesperado em /proc/stat.");
        return -1;
    }

    times->idle = idle + (count >= 5 ? iowait : 0);
    times->total = user + nice + system + idle + iowait + irq + softirq + steal;
    return 0;
}

/* Calcula uso entre duas amostras; nunca tenta derivar porcentagem de uma amostra isolada. */
double cpu_linux_usage_percent(const CpuTimes *previous, const CpuTimes *current) {
    if (previous == NULL || current == NULL || current->total <= previous->total) {
        return 0.0;
    }

    uint64_t total_delta = current->total - previous->total;
    uint64_t idle_delta = current->idle >= previous->idle ? current->idle - previous->idle : 0;
    double usage = 100.0 * (1.0 - (double)idle_delta / (double)total_delta);

    if (usage < 0.0) {
        return 0.0;
    }
    if (usage > 100.0) {
        return 100.0;
    }
    return usage;
}

/* ------------------------------------------------------------------------- */
/* cpufreq discovery                                                          */
/* ------------------------------------------------------------------------- */

/* Descobre a interseção de frequências suportadas por todas as políticas para evitar valores inválidos. */
int cpu_linux_detect_frequency_range(uint64_t *min_khz,
                                     uint64_t *max_khz,
                                     size_t *policy_count,
                                     char *error,
                                     size_t error_size) {
    if (min_khz == NULL || max_khz == NULL) {
        set_error(error, error_size, "Destinos inválidos para a faixa de frequência.");
        return -1;
    }

    DIR *directory = opendir(CPUFREQ_PATH);
    if (directory == NULL) {
        if (error != NULL && error_size > 0) {
            snprintf(error,
                     error_size,
                     "Não foi possível abrir %s: %s",
                     CPUFREQ_PATH,
                     strerror(errno));
        }
        return -1;
    }

    uint64_t common_min = 0;
    uint64_t common_max = UINT64_MAX;
    size_t count = 0;
    struct dirent *entry = NULL;

    while ((entry = readdir(directory)) != NULL) {
        if (!is_policy_name(entry->d_name)) {
            continue;
        }

        char min_path[512];
        char max_path[512];
        if (build_policy_file_path(min_path, sizeof(min_path), entry->d_name, "cpuinfo_min_freq") != 0 ||
            build_policy_file_path(max_path, sizeof(max_path), entry->d_name, "cpuinfo_max_freq") != 0) {
            continue;
        }

        uint64_t policy_min = 0;
        uint64_t policy_max = 0;
        if (read_u64_file(min_path, &policy_min) != 0 ||
            read_u64_file(max_path, &policy_max) != 0 ||
            policy_min > policy_max) {
            continue;
        }

        if (count == 0 || policy_min > common_min) {
            common_min = policy_min;
        }
        if (count == 0 || policy_max < common_max) {
            common_max = policy_max;
        }
        ++count;
    }
    closedir(directory);

    if (count == 0 || common_min > common_max) {
        set_error(error, error_size, "Nenhuma política cpufreq utilizável foi encontrada.");
        return -1;
    }

    *min_khz = common_min;
    *max_khz = common_max;
    if (policy_count != NULL) {
        *policy_count = count;
    }
    return 0;
}

/* Obtém uma frequência representativa da primeira política disponível para exibição na GUI. */
int cpu_linux_read_current_frequency(uint64_t *frequency_khz,
                                     char *error,
                                     size_t error_size) {
    if (frequency_khz == NULL) {
        set_error(error, error_size, "Destino inválido para a frequência atual.");
        return -1;
    }

    DIR *directory = opendir(CPUFREQ_PATH);
    if (directory == NULL) {
        set_error(error, error_size, "Não foi possível acessar as políticas cpufreq.");
        return -1;
    }

    int result = -1;
    struct dirent *entry = NULL;
    while ((entry = readdir(directory)) != NULL && result != 0) {
        if (!is_policy_name(entry->d_name)) {
            continue;
        }

        char path[512];
        if (build_policy_file_path(path, sizeof(path), entry->d_name, "scaling_cur_freq") == 0 &&
            read_u64_file(path, frequency_khz) == 0) {
            result = 0;
            break;
        }

        if (build_policy_file_path(path, sizeof(path), entry->d_name, "cpuinfo_cur_freq") == 0 &&
            read_u64_file(path, frequency_khz) == 0) {
            result = 0;
        }
    }
    closedir(directory);

    if (result != 0) {
        set_error(error, error_size, "Não foi possível ler a frequência atual.");
    }
    return result;
}

/* ------------------------------------------------------------------------- */
/* cpufreq writes                                                             */
/* ------------------------------------------------------------------------- */

/* Fixa min/max da política no mesmo valor, respeitando primeiro a ordem de escrita segura. */
static int set_one_policy_frequency(const char *policy_name,
                                    uint64_t target_khz,
                                    uint64_t low_frequency_khz) {
    char path[512];
    uint64_t hardware_min = 0;
    uint64_t hardware_max = 0;

    if (build_policy_file_path(path, sizeof(path), policy_name, "cpuinfo_min_freq") != 0 ||
        read_u64_file(path, &hardware_min) != 0) {
        return -1;
    }
    if (build_policy_file_path(path, sizeof(path), policy_name, "cpuinfo_max_freq") != 0 ||
        read_u64_file(path, &hardware_max) != 0 ||
        hardware_min > hardware_max) {
        return -1;
    }

    uint64_t frequency = target_khz;
    if (frequency < hardware_min) {
        frequency = hardware_min;
    }
    if (frequency > hardware_max) {
        frequency = hardware_max;
    }

    char min_path[512];
    char max_path[512];
    if (build_policy_file_path(min_path, sizeof(min_path), policy_name, "scaling_min_freq") != 0 ||
        build_policy_file_path(max_path, sizeof(max_path), policy_name, "scaling_max_freq") != 0) {
        return -1;
    }

    /*
     * When collapsing min/max to one value, write the bound that moves in the
     * safe direction first. Otherwise some cpufreq drivers briefly see
     * min > max and reject the write with EINVAL.
     */
    if (target_khz <= low_frequency_khz) {
        if (write_u64_file(min_path, frequency) != 0) {
            return -1;
        }
        return write_u64_file(max_path, frequency);
    }

    if (write_u64_file(max_path, frequency) != 0) {
        return -1;
    }
    return write_u64_file(min_path, frequency);
}

/* Aplica o mesmo alvo a todas as políticas cpufreq encontradas no sistema. */
int cpu_linux_set_all_policy_frequency(uint64_t target_khz,
                                       uint64_t low_frequency_khz,
                                       char *error,
                                       size_t error_size) {
    DIR *directory = opendir(CPUFREQ_PATH);
    if (directory == NULL) {
        set_error(error, error_size, "Não foi possível acessar as políticas cpufreq.");
        return -1;
    }

    size_t attempted = 0;
    size_t succeeded = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(directory)) != NULL) {
        if (!is_policy_name(entry->d_name)) {
            continue;
        }
        ++attempted;
        if (set_one_policy_frequency(entry->d_name, target_khz, low_frequency_khz) == 0) {
            ++succeeded;
        }
    }
    closedir(directory);

    if (attempted == 0 || succeeded != attempted) {
        if (error != NULL && error_size > 0) {
            snprintf(error,
                     error_size,
                     "Frequência aplicada em %zu de %zu políticas cpufreq.",
                     succeeded,
                     attempted);
        }
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* CPU vendor                                                                 */
/* ------------------------------------------------------------------------- */

/* Verifica GenuineIntel antes de oferecer controles específicos de intel-undervolt. */
int cpu_linux_is_intel(void) {
    FILE *file = fopen("/proc/cpuinfo", "r");
    if (file == NULL) {
        return 0;
    }

    char line[512];
    int intel = 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strstr(line, "vendor_id") != NULL && strstr(line, "GenuineIntel") != NULL) {
            intel = 1;
            break;
        }
    }
    fclose(file);
    return intel;
}
