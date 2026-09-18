#define _GNU_SOURCE

#include "config.h"

#include <gtk/gtk.h>

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CONFIG_PATH "/etc/cpu-clock-switch.json"
#define SERVICE_NAME "cpu-clock-switch.service"
#define CPUFREQ_PATH "/sys/devices/system/cpu/cpufreq"
#define UV_CONFIG_PATH "/etc/intel-undervolt.conf"
#define UV_SERVICE_NAME "intel-undervolt.service"

static const char *UV_NAMES[5] = {
    "CPU",
    "GPU",
    "CPU Cache",
    "System Agent",
    "Analog I/O",
};

typedef struct {
    uint64_t idle;
    uint64_t total;
} CpuTimes;

typedef struct {
    GtkWidget *window;
    GtkWidget *status_label;
    GtkWidget *usage_label;
    GtkWidget *frequency_label;
    GtkWidget *range_label;
    GtkWidget *service_label;
    GtkWidget *low_threshold;
    GtkWidget *high_threshold;
    GtkWidget *low_frequency;
    GtkWidget *high_frequency;
    GtkWidget *interval;
    GtkWidget *uv_frame;
    GtkWidget *uv_backend_label;
    GtkWidget *uv_spins[5];
    GtkWidget *uv_boot;
    GtkWidget *uv_apply_button;

    CpuConfig config;
    uint64_t min_khz;
    uint64_t max_khz;
    gboolean have_range;
    CpuTimes previous_times;
    gboolean have_previous_times;
    char *intel_undervolt_path;
    guint metrics_timer;
    guint service_timer;
} AppState;

static void set_status(AppState *state, const char *message, gboolean error) {
    gtk_label_set_text(GTK_LABEL(state->status_label), message == NULL ? "" : message);
    gtk_widget_remove_css_class(state->status_label, "error");
    gtk_widget_remove_css_class(state->status_label, "accent");
    gtk_widget_add_css_class(state->status_label, error ? "error" : "accent");
}

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

static gboolean detect_common_range(uint64_t *min_khz, uint64_t *max_khz) {
    GError *error = NULL;
    GDir *directory = g_dir_open(CPUFREQ_PATH, 0, &error);
    if (directory == NULL) {
        g_clear_error(&error);
        return FALSE;
    }

    gboolean found = FALSE;
    uint64_t common_min = 0;
    uint64_t common_max = UINT64_MAX;
    const char *name = NULL;

    while ((name = g_dir_read_name(directory)) != NULL) {
        if (!g_str_has_prefix(name, "policy")) {
            continue;
        }
        char *min_path = g_build_filename(CPUFREQ_PATH, name, "cpuinfo_min_freq", NULL);
        char *max_path = g_build_filename(CPUFREQ_PATH, name, "cpuinfo_max_freq", NULL);
        uint64_t policy_min = 0;
        uint64_t policy_max = 0;
        if (read_u64_file(min_path, &policy_min) == 0 &&
            read_u64_file(max_path, &policy_max) == 0 &&
            policy_min <= policy_max) {
            if (!found || policy_min > common_min) {
                common_min = policy_min;
            }
            if (!found || policy_max < common_max) {
                common_max = policy_max;
            }
            found = TRUE;
        }
        g_free(min_path);
        g_free(max_path);
    }
    g_dir_close(directory);

    if (!found || common_min > common_max) {
        return FALSE;
    }
    *min_khz = common_min;
    *max_khz = common_max;
    return TRUE;
}

static gboolean read_current_frequency(uint64_t *frequency_khz) {
    GError *error = NULL;
    GDir *directory = g_dir_open(CPUFREQ_PATH, 0, &error);
    if (directory == NULL) {
        g_clear_error(&error);
        return FALSE;
    }

    const char *name = NULL;
    gboolean found = FALSE;
    while ((name = g_dir_read_name(directory)) != NULL && !found) {
        if (!g_str_has_prefix(name, "policy")) {
            continue;
        }
        char *current_path = g_build_filename(CPUFREQ_PATH, name, "scaling_cur_freq", NULL);
        found = read_u64_file(current_path, frequency_khz) == 0;
        g_free(current_path);
        if (!found) {
            current_path = g_build_filename(CPUFREQ_PATH, name, "cpuinfo_cur_freq", NULL);
            found = read_u64_file(current_path, frequency_khz) == 0;
            g_free(current_path);
        }
    }
    g_dir_close(directory);
    return found;
}

static gboolean read_cpu_times(CpuTimes *times) {
    FILE *file = fopen("/proc/stat", "r");
    if (file == NULL) {
        return FALSE;
    }
    char line[1024];
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return FALSE;
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
        return FALSE;
    }
    times->idle = idle + (count >= 5 ? iowait : 0);
    times->total = user + nice + system + idle + iowait + irq + softirq + steal;
    return TRUE;
}

static char *format_frequency(uint64_t khz) {
    if (khz >= 1000000) {
        return g_strdup_printf("%.2f GHz", (double)khz / 1000000.0);
    }
    return g_strdup_printf("%.0f MHz", (double)khz / 1000.0);
}

static gboolean spawn_command(char **argv, char **stdout_text, char **stderr_text, int *exit_status) {
    GError *error = NULL;
    int wait_status = 0;
    gboolean ok = g_spawn_sync(NULL,
                               argv,
                               NULL,
                               G_SPAWN_SEARCH_PATH,
                               NULL,
                               NULL,
                               stdout_text,
                               stderr_text,
                               &wait_status,
                               &error);
    if (!ok) {
        if (stderr_text != NULL && *stderr_text == NULL) {
            *stderr_text = g_strdup(error->message);
        }
        g_clear_error(&error);
        return FALSE;
    }

    if (exit_status != NULL) {
        GError *status_error = NULL;
        if (!g_spawn_check_wait_status(wait_status, &status_error)) {
            *exit_status = 1;
            g_clear_error(&status_error);
        } else {
            *exit_status = 0;
        }
    }
    return TRUE;
}

static char *systemctl_state(const char *verb, const char *unit) {
    char *argv[] = {"systemctl", (char *)verb, (char *)unit, NULL};
    char *out = NULL;
    char *err = NULL;
    int status = 1;
    if (!spawn_command(argv, &out, &err, &status)) {
        g_free(out);
        g_free(err);
        return g_strdup("indisponível");
    }
    g_free(err);
    if (out == NULL) {
        return g_strdup(status == 0 ? "ok" : "indisponível");
    }
    g_strstrip(out);
    if (*out == '\0') {
        g_free(out);
        return g_strdup(status == 0 ? "ok" : "indisponível");
    }
    return out;
}

static gboolean run_pkexec(char **command, char **detail) {
    GPtrArray *args = g_ptr_array_new();
    g_ptr_array_add(args, "/usr/bin/pkexec");
    for (size_t i = 0; command[i] != NULL; ++i) {
        g_ptr_array_add(args, command[i]);
    }
    g_ptr_array_add(args, NULL);

    char *out = NULL;
    char *err = NULL;
    int status = 1;
    gboolean spawned = spawn_command((char **)args->pdata, &out, &err, &status);
    g_ptr_array_free(args, TRUE);
    g_free(out);

    if (!spawned || status != 0) {
        if (detail != NULL) {
            if (err != NULL && *g_strstrip(err) != '\0') {
                *detail = g_strdup(err);
            } else {
                *detail = g_strdup("A autorização administrativa foi cancelada ou o comando falhou.");
            }
        }
        g_free(err);
        return FALSE;
    }
    g_free(err);
    return TRUE;
}

static void normalize_config_to_range(AppState *state) {
    if (!state->have_range) {
        return;
    }
    if (state->config.low_frequency_khz < state->min_khz ||
        state->config.low_frequency_khz > state->max_khz) {
        state->config.low_frequency_khz = state->min_khz;
    }
    if (state->config.high_frequency_khz < state->min_khz ||
        state->config.high_frequency_khz > state->max_khz) {
        state->config.high_frequency_khz = state->max_khz;
    }
    if (state->config.low_frequency_khz > state->config.high_frequency_khz) {
        state->config.low_frequency_khz = state->min_khz;
        state->config.high_frequency_khz = state->max_khz;
    }
}

static void update_frequency_widgets(AppState *state) {
    if (!state->have_range) {
        gtk_widget_set_sensitive(state->low_frequency, FALSE);
        gtk_widget_set_sensitive(state->high_frequency, FALSE);
        gtk_label_set_text(GTK_LABEL(state->range_label), "Faixa cpufreq não detectada");
        return;
    }

    double min_mhz = (double)state->min_khz / 1000.0;
    double max_mhz = (double)state->max_khz / 1000.0;
    gtk_spin_button_set_range(GTK_SPIN_BUTTON(state->low_frequency), min_mhz, max_mhz);
    gtk_spin_button_set_range(GTK_SPIN_BUTTON(state->high_frequency), min_mhz, max_mhz);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->low_frequency),
                              (double)state->config.low_frequency_khz / 1000.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->high_frequency),
                              (double)state->config.high_frequency_khz / 1000.0);
    gtk_widget_set_sensitive(state->low_frequency, TRUE);
    gtk_widget_set_sensitive(state->high_frequency, TRUE);

    char *min_text = format_frequency(state->min_khz);
    char *max_text = format_frequency(state->max_khz);
    char *range_text = g_strdup_printf("Faixa comum: %s — %s", min_text, max_text);
    gtk_label_set_text(GTK_LABEL(state->range_label), range_text);
    g_free(range_text);
    g_free(min_text);
    g_free(max_text);
}

static void load_widgets_from_config(AppState *state) {
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->low_threshold), state->config.low_threshold_pct);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->high_threshold), state->config.high_threshold_pct);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->interval), (double)state->config.interval_ms);
    update_frequency_widgets(state);
}

static void read_config_from_widgets(AppState *state) {
    state->config.low_threshold_pct = gtk_spin_button_get_value(GTK_SPIN_BUTTON(state->low_threshold));
    state->config.high_threshold_pct = gtk_spin_button_get_value(GTK_SPIN_BUTTON(state->high_threshold));
    state->config.low_frequency_khz =
        (uint64_t)(gtk_spin_button_get_value(GTK_SPIN_BUTTON(state->low_frequency)) * 1000.0 + 0.5);
    state->config.high_frequency_khz =
        (uint64_t)(gtk_spin_button_get_value(GTK_SPIN_BUTTON(state->high_frequency)) * 1000.0 + 0.5);
    state->config.interval_ms =
        (uint64_t)(gtk_spin_button_get_value(GTK_SPIN_BUTTON(state->interval)) + 0.5);
}

static gboolean save_frequency_config(AppState *state, gboolean restart) {
    if (!state->have_range) {
        set_status(state, "Não há uma faixa cpufreq válida para salvar.", TRUE);
        return FALSE;
    }

    read_config_from_widgets(state);
    char error[256] = {0};
    if (cpu_config_validate(&state->config,
                            state->min_khz,
                            state->max_khz,
                            1,
                            error,
                            sizeof(error)) != 0) {
        set_status(state, error, TRUE);
        return FALSE;
    }

    char *temp_path = NULL;
    GError *tmp_error = NULL;
    int fd = g_file_open_tmp("cpu-clock-switch-XXXXXX", &temp_path, &tmp_error);
    if (fd < 0) {
        set_status(state, tmp_error->message, TRUE);
        g_clear_error(&tmp_error);
        return FALSE;
    }
    close(fd);

    if (cpu_config_write(temp_path, &state->config, error, sizeof(error)) != 0) {
        set_status(state, error, TRUE);
        unlink(temp_path);
        g_free(temp_path);
        return FALSE;
    }

    char *install_command[] = {"/usr/bin/install", "-m", "0644", temp_path, CONFIG_PATH, NULL};
    char *detail = NULL;
    gboolean ok = run_pkexec(install_command, &detail);
    unlink(temp_path);
    g_free(temp_path);
    if (!ok) {
        set_status(state, detail, TRUE);
        g_free(detail);
        return FALSE;
    }
    g_free(detail);

    if (restart) {
        char *restart_command[] = {"/usr/bin/systemctl", "restart", SERVICE_NAME, NULL};
        if (!run_pkexec(restart_command, &detail)) {
            set_status(state, detail, TRUE);
            g_free(detail);
            return FALSE;
        }
        g_free(detail);
        set_status(state, "Configuração salva e serviço reiniciado.", FALSE);
    } else {
        set_status(state, "Configuração salva. Reinicie o serviço para aplicar.", FALSE);
    }
    return TRUE;
}

static gboolean is_intel_cpu(void) {
    char *contents = NULL;
    gsize length = 0;
    if (!g_file_get_contents("/proc/cpuinfo", &contents, &length, NULL)) {
        return FALSE;
    }
    gboolean intel = strstr(contents, "GenuineIntel") != NULL;
    g_free(contents);
    return intel;
}

static gboolean parse_uv_line(const char *line, int *index, double *value) {
    int parsed_index = -1;
    if (sscanf(line, " undervolt %d", &parsed_index) != 1 &&
        sscanf(line, "undervolt %d", &parsed_index) != 1) {
        return FALSE;
    }
    if (parsed_index < 0 || parsed_index >= 5) {
        return FALSE;
    }

    const char *end = line + strlen(line);
    while (end > line && isspace((unsigned char)end[-1])) {
        --end;
    }
    const char *start = end;
    while (start > line && !isspace((unsigned char)start[-1])) {
        --start;
    }
    char number[64];
    size_t size = (size_t)(end - start);
    if (size == 0 || size >= sizeof(number)) {
        return FALSE;
    }
    memcpy(number, start, size);
    number[size] = '\0';

    char *number_end = NULL;
    errno = 0;
    double parsed_value = strtod(number, &number_end);
    if (errno != 0 || number_end == number || *number_end != '\0') {
        return FALSE;
    }

    *index = parsed_index;
    *value = parsed_value;
    return TRUE;
}

static void load_uv_values(double values[5]) {
    for (int i = 0; i < 5; ++i) {
        values[i] = 0.0;
    }

    char *contents = NULL;
    if (!g_file_get_contents(UV_CONFIG_PATH, &contents, NULL, NULL)) {
        return;
    }
    char **lines = g_strsplit(contents, "\n", -1);
    for (int i = 0; lines[i] != NULL; ++i) {
        int index = -1;
        double value = 0.0;
        if (parse_uv_line(lines[i], &index, &value)) {
            values[index] = value;
        }
    }
    g_strfreev(lines);
    g_free(contents);
}

static char *build_uv_config(const double values[5]) {
    char *contents = NULL;
    gboolean have_existing = g_file_get_contents(UV_CONFIG_PATH, &contents, NULL, NULL);
    if (!have_existing) {
        contents = g_strdup("enable no\n\ninterval 5000\ndaemon undervolt:once\n");
    }

    gboolean replaced[5] = {FALSE, FALSE, FALSE, FALSE, FALSE};
    GString *result = g_string_new(NULL);
    char **lines = g_strsplit(contents, "\n", -1);
    for (int i = 0; lines[i] != NULL; ++i) {
        int index = -1;
        double old_value = 0.0;
        if (parse_uv_line(lines[i], &index, &old_value)) {
            g_string_append_printf(result,
                                   "undervolt %d '%s' %.2f\n",
                                   index,
                                   UV_NAMES[index],
                                   values[index]);
            replaced[index] = TRUE;
        } else if (lines[i + 1] != NULL || *lines[i] != '\0') {
            g_string_append(result, lines[i]);
            g_string_append_c(result, '\n');
        }
    }
    for (int i = 0; i < 5; ++i) {
        if (!replaced[i]) {
            g_string_append_printf(result,
                                   "undervolt %d '%s' %.2f\n",
                                   i,
                                   UV_NAMES[i],
                                   values[i]);
        }
    }
    g_strfreev(lines);
    g_free(contents);
    return g_string_free(result, FALSE);
}

static gboolean uv_service_enabled(void) {
    char *state = systemctl_state("is-enabled", UV_SERVICE_NAME);
    gboolean enabled = g_strcmp0(state, "enabled") == 0;
    g_free(state);
    return enabled;
}

static void refresh_uv_widgets(AppState *state) {
    gboolean intel = is_intel_cpu();
    g_clear_pointer(&state->intel_undervolt_path, g_free);
    state->intel_undervolt_path = access("/usr/bin/intel-undervolt", X_OK) == 0
        ? g_strdup("/usr/bin/intel-undervolt")
        : NULL;
    gboolean available = intel && state->intel_undervolt_path != NULL;

    if (!intel) {
        gtk_label_set_text(GTK_LABEL(state->uv_backend_label),
                           "Indisponível: processador Intel não detectado.");
    } else if (state->intel_undervolt_path == NULL) {
        gtk_label_set_text(GTK_LABEL(state->uv_backend_label),
                           "Backend ausente: instale intel-undervolt para habilitar este painel.");
    } else {
        gtk_label_set_text(GTK_LABEL(state->uv_backend_label),
                           "Backend: intel-undervolt detectado. O firmware ainda pode bloquear ajustes.");
    }

    gtk_widget_set_sensitive(state->uv_apply_button, available);
    gtk_widget_set_sensitive(state->uv_boot, available);
    for (int i = 0; i < 5; ++i) {
        gtk_widget_set_sensitive(state->uv_spins[i], available);
    }

    if (available) {
        double values[5];
        load_uv_values(values);
        for (int i = 0; i < 5; ++i) {
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->uv_spins[i]), values[i]);
        }
        gtk_check_button_set_active(GTK_CHECK_BUTTON(state->uv_boot), uv_service_enabled());
    }
}

static void apply_undervolt(AppState *state) {
    if (state->intel_undervolt_path == NULL || !is_intel_cpu()) {
        set_status(state, "intel-undervolt não está disponível para esta máquina.", TRUE);
        return;
    }

    double values[5];
    for (int i = 0; i < 5; ++i) {
        values[i] = gtk_spin_button_get_value(GTK_SPIN_BUTTON(state->uv_spins[i]));
        if (values[i] > 0.0001 || values[i] < -150.0) {
            set_status(state, "Os offsets de tensão devem ficar entre -150 e 0 mV.", TRUE);
            return;
        }
    }

    char *new_config = build_uv_config(values);
    char *temp_path = NULL;
    GError *error = NULL;
    int fd = g_file_open_tmp("intel-undervolt-XXXXXX", &temp_path, &error);
    if (fd < 0) {
        set_status(state, error->message, TRUE);
        g_clear_error(&error);
        g_free(new_config);
        return;
    }

    FILE *temp = fdopen(fd, "w");
    gboolean write_ok = temp != NULL;
    if (temp != NULL) {
        if (fputs(new_config, temp) == EOF) {
            write_ok = FALSE;
        }
        if (fclose(temp) != 0) {
            write_ok = FALSE;
        }
    } else {
        close(fd);
    }
    if (!write_ok) {
        set_status(state, "Não foi possível preparar a configuração de tensão.", TRUE);
        unlink(temp_path);
        g_free(temp_path);
        g_free(new_config);
        return;
    }
    g_free(new_config);

    char *install_command[] = {"/usr/bin/install", "-m", "0644", temp_path, UV_CONFIG_PATH, NULL};
    char *detail = NULL;
    gboolean ok = run_pkexec(install_command, &detail);
    unlink(temp_path);
    g_free(temp_path);
    if (!ok) {
        set_status(state, detail, TRUE);
        g_free(detail);
        return;
    }
    g_free(detail);

    char *apply_command[] = {state->intel_undervolt_path, "apply", NULL};
    if (!run_pkexec(apply_command, &detail)) {
        char *message = g_strdup_printf("Falha ao aplicar tensão: %s", detail);
        set_status(state, message, TRUE);
        g_free(message);
        g_free(detail);
        return;
    }
    g_free(detail);

    gboolean enable_at_boot = gtk_check_button_get_active(GTK_CHECK_BUTTON(state->uv_boot));
    char *enable_command[] = {"/usr/bin/systemctl", "enable", UV_SERVICE_NAME, NULL};
    char *disable_command[] = {"/usr/bin/systemctl", "disable", UV_SERVICE_NAME, NULL};
    if (!run_pkexec(enable_at_boot ? enable_command : disable_command, &detail)) {
        char *message = g_strdup_printf("Tensão aplicada, mas não foi possível alterar o boot: %s", detail);
        set_status(state, message, TRUE);
        g_free(message);
        g_free(detail);
        return;
    }
    g_free(detail);

    set_status(state,
               "Offsets de tensão salvos e aplicados. Teste a estabilidade antes de reduzir mais.",
               FALSE);
}

static gboolean refresh_metrics(gpointer user_data) {
    AppState *state = user_data;
    CpuTimes current;
    if (read_cpu_times(&current)) {
        if (state->have_previous_times) {
            uint64_t total_delta = current.total - state->previous_times.total;
            uint64_t idle_delta = current.idle - state->previous_times.idle;
            if (total_delta > 0) {
                double usage = 100.0 * (1.0 - (double)idle_delta / (double)total_delta);
                char *usage_text = g_strdup_printf("Uso atual: %.1f%%", usage);
                gtk_label_set_text(GTK_LABEL(state->usage_label), usage_text);
                g_free(usage_text);
            }
        }
        state->previous_times = current;
        state->have_previous_times = TRUE;
    }

    uint64_t frequency = 0;
    if (read_current_frequency(&frequency)) {
        char *formatted = format_frequency(frequency);
        char *text = g_strdup_printf("Frequência atual: %s", formatted);
        gtk_label_set_text(GTK_LABEL(state->frequency_label), text);
        g_free(text);
        g_free(formatted);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean refresh_service(gpointer user_data) {
    AppState *state = user_data;
    char *service = systemctl_state("is-active", SERVICE_NAME);
    char *text = g_strdup_printf("Serviço: %s", service);
    gtk_label_set_text(GTK_LABEL(state->service_label), text);
    g_free(text);
    g_free(service);
    return G_SOURCE_CONTINUE;
}

static void on_save_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    save_frequency_config(user_data, FALSE);
}

static void on_save_restart_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    save_frequency_config(user_data, TRUE);
}

static void on_defaults_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *state = user_data;
    cpu_config_defaults(&state->config);
    if (state->have_range) {
        state->config.low_frequency_khz = state->min_khz;
        state->config.high_frequency_khz = state->max_khz;
    }
    load_widgets_from_config(state);
    set_status(state, "Padrões restaurados na interface; salve para aplicar.", FALSE);
}

static void on_refresh_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *state = user_data;
    state->have_range = detect_common_range(&state->min_khz, &state->max_khz);
    normalize_config_to_range(state);
    update_frequency_widgets(state);
    refresh_uv_widgets(state);
    set_status(state, "Detecção de hardware atualizada.", FALSE);
}

static void on_uv_apply_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    apply_undervolt(user_data);
}

static GtkWidget *make_spin_row(GtkWidget *grid,
                                int row,
                                const char *label_text,
                                double min,
                                double max,
                                double step,
                                int digits) {
    GtkWidget *label = gtk_label_new(label_text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    GtkWidget *spin = gtk_spin_button_new_with_range(min, max, step);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), digits);
    gtk_widget_set_hexpand(spin, TRUE);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), spin, 1, row, 1, 1);
    return spin;
}

static GtkWidget *build_frequency_frame(AppState *state) {
    GtkWidget *frame = gtk_frame_new("Controle automático de frequência");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_frame_set_child(GTK_FRAME(frame), box);

    GtkWidget *description = gtk_label_new(
        "O daemon alterna entre dois clocks usando histerese de uso total da CPU.");
    gtk_label_set_wrap(GTK_LABEL(description), TRUE);
    gtk_label_set_xalign(GTK_LABEL(description), 0.0f);
    gtk_box_append(GTK_BOX(box), description);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_box_append(GTK_BOX(box), grid);

    state->low_threshold = make_spin_row(grid, 0, "Baixa carga abaixo de (%)", 0, 99, 1, 0);
    state->high_threshold = make_spin_row(grid, 1, "Alta carga acima de (%)", 1, 100, 1, 0);
    state->low_frequency = make_spin_row(grid, 2, "Frequência baixa (MHz)", 100, 10000, 100, 0);
    state->high_frequency = make_spin_row(grid, 3, "Frequência alta (MHz)", 100, 10000, 100, 0);
    state->interval = make_spin_row(grid, 4, "Intervalo de leitura (ms)", 100, 5000, 100, 0);

    state->range_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(state->range_label), 0.0f);
    gtk_box_append(GTK_BOX(box), state->range_label);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(box), buttons);
    GtkWidget *save = gtk_button_new_with_label("Salvar configuração");
    GtkWidget *save_restart = gtk_button_new_with_label("Salvar e reiniciar serviço");
    GtkWidget *defaults = gtk_button_new_with_label("Restaurar padrões");
    GtkWidget *refresh = gtk_button_new_with_label("Atualizar detecção");
    gtk_box_append(GTK_BOX(buttons), save);
    gtk_box_append(GTK_BOX(buttons), save_restart);
    gtk_box_append(GTK_BOX(buttons), defaults);
    gtk_box_append(GTK_BOX(buttons), refresh);
    g_signal_connect(save, "clicked", G_CALLBACK(on_save_clicked), state);
    g_signal_connect(save_restart, "clicked", G_CALLBACK(on_save_restart_clicked), state);
    g_signal_connect(defaults, "clicked", G_CALLBACK(on_defaults_clicked), state);
    g_signal_connect(refresh, "clicked", G_CALLBACK(on_refresh_clicked), state);
    return frame;
}

static GtkWidget *build_uv_frame(AppState *state) {
    GtkWidget *frame = gtk_frame_new("Tensão / undervolt Intel");
    state->uv_frame = frame;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_frame_set_child(GTK_FRAME(frame), box);

    state->uv_backend_label = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(state->uv_backend_label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(state->uv_backend_label), 0.0f);
    gtk_box_append(GTK_BOX(box), state->uv_backend_label);

    GtkWidget *warning = gtk_label_new(
        "Somente offsets entre -150 e 0 mV são aceitos. Valores instáveis podem causar travamentos ou reinicializações.");
    gtk_label_set_wrap(GTK_LABEL(warning), TRUE);
    gtk_label_set_xalign(GTK_LABEL(warning), 0.0f);
    gtk_widget_add_css_class(warning, "warning");
    gtk_box_append(GTK_BOX(box), warning);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_box_append(GTK_BOX(box), grid);

    const char *labels[5] = {
        "CPU Core (mV)",
        "GPU integrada (mV)",
        "CPU Cache (mV)",
        "System Agent (mV)",
        "Analog I/O (mV)",
    };
    for (int i = 0; i < 5; ++i) {
        state->uv_spins[i] = make_spin_row(grid, i, labels[i], -150.0, 0.0, 1.0, 2);
    }

    state->uv_boot = gtk_check_button_new_with_label("Aplicar automaticamente no boot");
    gtk_box_append(GTK_BOX(box), state->uv_boot);

    state->uv_apply_button = gtk_button_new_with_label("Salvar e aplicar tensão");
    gtk_widget_add_css_class(state->uv_apply_button, "suggested-action");
    gtk_box_append(GTK_BOX(box), state->uv_apply_button);
    g_signal_connect(state->uv_apply_button, "clicked", G_CALLBACK(on_uv_apply_clicked), state);
    return frame;
}

static void on_window_destroy(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppState *state = user_data;
    if (state->metrics_timer != 0) {
        g_source_remove(state->metrics_timer);
    }
    if (state->service_timer != 0) {
        g_source_remove(state->service_timer);
    }
    g_free(state->intel_undervolt_path);
    g_free(state);
}

static void activate(GtkApplication *application, gpointer user_data) {
    (void)user_data;
    AppState *state = g_new0(AppState, 1);
    cpu_config_defaults(&state->config);

    char error[256] = {0};
    cpu_config_load(CONFIG_PATH, &state->config, error, sizeof(error));
    state->have_range = detect_common_range(&state->min_khz, &state->max_khz);
    normalize_config_to_range(state);

    state->window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(state->window), "CPU Switch Control");
    gtk_window_set_default_size(GTK_WINDOW(state->window), 760, 760);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_window_set_child(GTK_WINDOW(state->window), scroll);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(root, 16);
    gtk_widget_set_margin_bottom(root, 16);
    gtk_widget_set_margin_start(root, 16);
    gtk_widget_set_margin_end(root, 16);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), root);

    GtkWidget *title = gtk_label_new("CPU Switch Control");
    gtk_widget_add_css_class(title, "title-1");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_box_append(GTK_BOX(root), title);

    GtkWidget *metrics = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    state->usage_label = gtk_label_new("Uso atual: —");
    state->frequency_label = gtk_label_new("Frequência atual: —");
    state->service_label = gtk_label_new("Serviço: —");
    gtk_box_append(GTK_BOX(metrics), state->usage_label);
    gtk_box_append(GTK_BOX(metrics), state->frequency_label);
    gtk_box_append(GTK_BOX(metrics), state->service_label);
    gtk_box_append(GTK_BOX(root), metrics);

    state->status_label = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(state->status_label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(state->status_label), 0.0f);
    gtk_box_append(GTK_BOX(root), state->status_label);

    gtk_box_append(GTK_BOX(root), build_frequency_frame(state));
    gtk_box_append(GTK_BOX(root), build_uv_frame(state));

    load_widgets_from_config(state);
    refresh_uv_widgets(state);
    refresh_metrics(state);
    refresh_service(state);

    if (*error != '\0') {
        set_status(state, error, TRUE);
    }

    state->metrics_timer = g_timeout_add(500, refresh_metrics, state);
    state->service_timer = g_timeout_add_seconds(2, refresh_service, state);
    g_signal_connect(state->window, "destroy", G_CALLBACK(on_window_destroy), state);
    gtk_window_present(GTK_WINDOW(state->window));
}

int main(int argc, char **argv) {
    GtkApplication *application = gtk_application_new("io.github.xoykor.cpu-switch-control",
                                                       G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return status;
}
