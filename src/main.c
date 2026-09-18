/*
 * CPU Switch Control - main.c
 *
 * Interface gráfica GTK4 do projeto. Este arquivo coordena os módulos de CPU,
 * configuração, comandos privilegiados e undervolt, mas evita implementar aqui
 * os detalhes de baixo nível de cada um deles.
 *
 * Organização:
 *   1. estado e helpers de interface;
 *   2. configuração de frequência;
 *   3. métricas e status do serviço;
 *   4. controle de tensão/undervolt;
 *   5. construção das páginas GTK;
 *   6. ciclo de vida da aplicação.
 */

#define _GNU_SOURCE

#include "command.h"
#include "config.h"
#include "cpu_linux.h"
#include "undervolt.h"

#include <gtk/gtk.h>
#include <glib/gstdio.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CONFIG_PATH "/etc/cpu-clock-switch.json"
#define SERVICE_NAME "cpu-clock-switch.service"
#define UNDERVOLT_CONFIG_PATH "/etc/intel-undervolt.conf"
#define UNDERVOLT_SERVICE_NAME "intel-undervolt.service"

/*
 * Widgets and runtime state live together so callbacks only need one pointer.
 * The GTK widgets themselves are owned by the window hierarchy; this structure
 * merely keeps convenient non-owning references to controls we update later.
 */
typedef struct {
    GtkWidget *window;
    GtkWidget *status_label;

    /* Frequency page */
    GtkWidget *service_label;
    GtkWidget *usage_label;
    GtkWidget *frequency_label;
    GtkWidget *range_label;
    GtkWidget *policy_label;
    GtkWidget *low_threshold_spin;
    GtkWidget *high_threshold_spin;
    GtkWidget *low_frequency_spin;
    GtkWidget *high_frequency_spin;
    GtkWidget *interval_spin;
    GtkWidget *frequency_controls;

    /* Undervolt page */
    GtkWidget *undervolt_backend_label;
    GtkWidget *undervolt_controls;
    GtkWidget *undervolt_spins[UNDERVOLT_DOMAIN_COUNT];
    GtkWidget *undervolt_domain_labels[UNDERVOLT_DOMAIN_COUNT];
    GtkWidget *undervolt_domain_units[UNDERVOLT_DOMAIN_COUNT];
    GtkWidget *undervolt_boot_check;
    unsigned char undervolt_present[UNDERVOLT_DOMAIN_COUNT];
    gboolean undervolt_domains_detected;

    CpuConfig config;
    uint64_t min_khz;
    uint64_t max_khz;
    size_t policy_count;
    gboolean have_frequency_range;

    CpuTimes previous_times;
    gboolean have_previous_times;

    char *undervolt_program;
    guint metrics_timer;
    guint service_timer;
} AppState;

/* ------------------------------------------------------------------------- */
/* Small UI helpers                                                           */
/* ------------------------------------------------------------------------- */

/* Cria labels alinhados à esquerda para manter o layout consistente em todas as páginas. */
static GtkWidget *new_left_label(const char *text) {
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    return label;
}

/* Cria um GtkSpinButton já configurado com faixa, passo, casas decimais e expansão horizontal. */
static GtkWidget *new_spin(double minimum,
                           double maximum,
                           double step,
                           unsigned int digits) {
    GtkWidget *spin = gtk_spin_button_new_with_range(minimum, maximum, step);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), digits);
    gtk_widget_set_hexpand(spin, TRUE);
    return spin;
}

/* Adiciona ao grid uma linha padrão formada por rótulo, controle e unidade opcional. */
static void grid_add_row(GtkWidget *grid,
                         int row,
                         const char *label_text,
                         GtkWidget *control,
                         const char *unit) {
    GtkWidget *label = new_left_label(label_text);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), control, 1, row, 1, 1);

    if (unit != NULL) {
        GtkWidget *unit_label = new_left_label(unit);
        gtk_grid_attach(GTK_GRID(grid), unit_label, 2, row, 1, 1);
    }
}

/* Atualiza a mensagem global de status e troca a classe CSS conforme sucesso ou erro. */
static void set_status(AppState *state, const char *message, gboolean is_error) {
    gtk_label_set_text(GTK_LABEL(state->status_label), message != NULL ? message : "");

    /* Named CSS classes keep status styling readable without custom CSS. */
    gtk_widget_remove_css_class(state->status_label, "error");
    gtk_widget_remove_css_class(state->status_label, "accent");
    gtk_widget_add_css_class(state->status_label, is_error ? "error" : "accent");
}

/* Converte kHz do kernel para uma string curta em MHz ou GHz apropriada para a interface. */
static char *format_frequency(uint64_t khz) {
    if (khz >= 1000000U) {
        return g_strdup_printf("%.2f GHz", (double)khz / 1000000.0);
    }
    return g_strdup_printf("%.0f MHz", (double)khz / 1000.0);
}

/* Escolhe a melhor mensagem de erro produzida por um comando externo para exibir ao usuário. */
static const char *command_failure_detail(const CommandResult *result) {
    if (result != NULL && result->stderr_text != NULL) {
        char *trimmed = g_strstrip(result->stderr_text);
        if (*trimmed != '\0') {
            return trimmed;
        }
    }
    return "O comando falhou ou a autorização foi cancelada.";
}

/* ------------------------------------------------------------------------- */
/* Frequency configuration                                                    */
/* ------------------------------------------------------------------------- */

/* Ajusta configuração antiga aos limites reais da CPU atual antes de preencher os controles. */
static void normalize_config_to_hardware(AppState *state) {
    if (!state->have_frequency_range) {
        return;
    }

    /*
     * Old configuration files can contain frequencies from another CPU.
     * Clamp them before presenting values in the GUI instead of silently
     * writing an invalid frequency later.
     */
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

/* Copia a configuração em memória para os campos visíveis da página de frequência. */
static void frequency_config_to_widgets(AppState *state) {
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->low_threshold_spin),
                              state->config.low_threshold_pct);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->high_threshold_spin),
                              state->config.high_threshold_pct);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->interval_spin),
                              (double)state->config.interval_ms);

    if (!state->have_frequency_range) {
        gtk_widget_set_sensitive(state->frequency_controls, FALSE);
        gtk_label_set_text(GTK_LABEL(state->range_label), "Faixa cpufreq não detectada");
        gtk_label_set_text(GTK_LABEL(state->policy_label), "Políticas: 0");
        return;
    }

    double min_mhz = (double)state->min_khz / 1000.0;
    double max_mhz = (double)state->max_khz / 1000.0;
    gtk_spin_button_set_range(GTK_SPIN_BUTTON(state->low_frequency_spin), min_mhz, max_mhz);
    gtk_spin_button_set_range(GTK_SPIN_BUTTON(state->high_frequency_spin), min_mhz, max_mhz);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->low_frequency_spin),
                              (double)state->config.low_frequency_khz / 1000.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->high_frequency_spin),
                              (double)state->config.high_frequency_khz / 1000.0);
    gtk_widget_set_sensitive(state->frequency_controls, TRUE);

    char *minimum = format_frequency(state->min_khz);
    char *maximum = format_frequency(state->max_khz);
    char *range_text = g_strdup_printf("Faixa comum: %s — %s", minimum, maximum);
    char *policy_text = g_strdup_printf("Políticas: %zu", state->policy_count);
    gtk_label_set_text(GTK_LABEL(state->range_label), range_text);
    gtk_label_set_text(GTK_LABEL(state->policy_label), policy_text);
    g_free(policy_text);
    g_free(range_text);
    g_free(maximum);
    g_free(minimum);
}

/* Lê os campos da GUI e converte MHz/%/ms para a estrutura usada pelo daemon. */
static void frequency_widgets_to_config(AppState *state) {
    state->config.low_threshold_pct =
        gtk_spin_button_get_value(GTK_SPIN_BUTTON(state->low_threshold_spin));
    state->config.high_threshold_pct =
        gtk_spin_button_get_value(GTK_SPIN_BUTTON(state->high_threshold_spin));
    state->config.low_frequency_khz =
        (uint64_t)(gtk_spin_button_get_value(GTK_SPIN_BUTTON(state->low_frequency_spin)) *
                       1000.0 +
                   0.5);
    state->config.high_frequency_khz =
        (uint64_t)(gtk_spin_button_get_value(GTK_SPIN_BUTTON(state->high_frequency_spin)) *
                       1000.0 +
                   0.5);
    state->config.interval_ms =
        (uint64_t)(gtk_spin_button_get_value(GTK_SPIN_BUTTON(state->interval_spin)) + 0.5);
}

/* Redetecta políticas cpufreq e atualiza faixa, quantidade de políticas e sensibilidade dos controles. */
static void refresh_hardware_detection(AppState *state, gboolean report_success) {
    char error[256] = {0};
    state->have_frequency_range =
        cpu_linux_detect_frequency_range(&state->min_khz,
                                         &state->max_khz,
                                         &state->policy_count,
                                         error,
                                         sizeof(error)) == 0;

    if (state->have_frequency_range) {
        normalize_config_to_hardware(state);
        frequency_config_to_widgets(state);
        if (report_success) {
            set_status(state, "Limites do processador atualizados.", FALSE);
        }
    } else {
        state->policy_count = 0;
        frequency_config_to_widgets(state);
        set_status(state, error, TRUE);
    }
}

/*
 * Saving is a two-step operation:
 *   1. create the new JSON as the unprivileged desktop user;
 *   2. use pkexec only for the final install into /etc.
 *
 * This keeps the GUI itself unprivileged and makes the privileged action small
 * and easy to audit.
 */
/*
 * Verifica se o systemd conhece a unit do daemon.
 *
 * "is-active" sozinho não distingue bem uma unit ausente de uma unit apenas
 * parada. "systemctl cat" falha somente quando a definição da unit não existe.
 */
static gboolean frequency_service_is_installed(void) {
    char *argv[] = {
        (char *)"systemctl",
        (char *)"cat",
        (char *)SERVICE_NAME,
        NULL,
    };

    CommandResult result;
    gboolean installed = command_run(argv, &result);
    command_result_clear(&result);
    return installed;
}

/*
 * Quando a GUI está rodando de um AppImage, CPU_SWITCH_APPDIR aponta para o
 * AppDir montado. O script de setup copia os componentes para um staging normal
 * em /tmp antes de chamar pkexec, evitando a limitação de acesso ao mount FUSE.
 */
static gboolean ensure_frequency_service(AppState *state, char **detail) {
    if (detail != NULL) {
        *detail = NULL;
    }

    if (frequency_service_is_installed()) {
        return TRUE;
    }

    const char *appdir = g_getenv("CPU_SWITCH_APPDIR");
    if (appdir == NULL || *appdir == '\0') {
        if (detail != NULL) {
            *detail = g_strdup(
                "O serviço cpu-clock-switch não está instalado. "
                "Reinstale o programa ou use o AppImage atualizado.");
        }
        return FALSE;
    }

    char *setup_script = g_build_filename(appdir,
                                          "usr",
                                          "lib",
                                          "cpu-switch-control",
                                          "ensure-system-components.sh",
                                          NULL);

    if (!g_file_test(setup_script, G_FILE_TEST_IS_EXECUTABLE)) {
        if (detail != NULL) {
            *detail = g_strdup(
                "O AppImage não contém o instalador do serviço. "
                "Use uma versão mais recente do CPU Switch Control.");
        }
        g_free(setup_script);
        return FALSE;
    }

    char *argv[] = {setup_script, NULL};
    CommandResult result;
    gboolean ok = command_run(argv, &result);

    if (!ok && detail != NULL) {
        *detail = g_strdup(command_failure_detail(&result));
    }
    command_result_clear(&result);
    g_free(setup_script);

    if (!ok) {
        return FALSE;
    }

    if (!frequency_service_is_installed()) {
        if (detail != NULL) {
            *detail = g_strdup(
                "A instalação terminou, mas o systemd ainda não encontrou "
                "cpu-clock-switch.service.");
        }
        return FALSE;
    }

    /*
     * Atualiza imediatamente o rótulo da GUI. O timer também faria isso depois,
     * mas a resposta instantânea deixa claro que o reparo funcionou.
     */
    char *service_state = command_systemctl_state("is-active", SERVICE_NAME);
    char *label = g_strdup_printf("Serviço: %s", service_state);
    gtk_label_set_text(GTK_LABEL(state->service_label), label);
    g_free(label);
    g_free(service_state);

    return TRUE;
}

/* Valida, grava em arquivo temporário e instala a configuração em /etc usando privilégio apenas no passo final. */
static gboolean save_frequency_config(AppState *state, gboolean restart_service) {
    if (!state->have_frequency_range) {
        set_status(state, "Não há uma faixa cpufreq válida para salvar.", TRUE);
        return FALSE;
    }

    frequency_widgets_to_config(state);

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

    char *temporary_path = NULL;
    GError *temporary_error = NULL;
    int fd = g_file_open_tmp("cpu-clock-switch-XXXXXX", &temporary_path, &temporary_error);
    if (fd < 0) {
        set_status(state,
                   temporary_error != NULL ? temporary_error->message
                                           : "Não foi possível criar arquivo temporário.",
                   TRUE);
        g_clear_error(&temporary_error);
        return FALSE;
    }
    close(fd);

    if (cpu_config_write(temporary_path, &state->config, error, sizeof(error)) != 0) {
        set_status(state, error, TRUE);
        g_unlink(temporary_path);
        g_free(temporary_path);
        return FALSE;
    }

    char *install_argv[] = {
        (char *)"install",
        (char *)"-m",
        (char *)"0644",
        temporary_path,
        (char *)CONFIG_PATH,
        NULL,
    };
    char *detail = NULL;
    gboolean installed = command_run_pkexec(install_argv, &detail);
    g_unlink(temporary_path);
    g_free(temporary_path);

    if (!installed) {
        set_status(state, detail, TRUE);
        g_free(detail);
        return FALSE;
    }
    g_free(detail);

    if (restart_service) {
        /*
         * AppImages antigos podiam abrir sem conseguir instalar a unit porque
         * pkexec não tinha acesso ao mount FUSE. Repara a integração aqui antes
         * de tentar reiniciar, para o botão funcionar também nesse cenário.
         */
        if (!ensure_frequency_service(state, &detail)) {
            set_status(state,
                       detail != NULL ? detail
                                      : "Não foi possível instalar o serviço de frequência.",
                       TRUE);
            g_free(detail);
            return FALSE;
        }
        g_free(detail);
        detail = NULL;

        char *restart_argv[] = {
            (char *)"systemctl",
            (char *)"restart",
            (char *)SERVICE_NAME,
            NULL,
        };
        if (!command_run_pkexec(restart_argv, &detail)) {
            set_status(state, detail, TRUE);
            g_free(detail);
            return FALSE;
        }
        g_free(detail);
        set_status(state, "Configuração salva e serviço reiniciado.", FALSE);
    } else {
        set_status(state, "Configuração salva. Reinicie o serviço para aplicá-la.", FALSE);
    }

    return TRUE;
}

/* Callback do botão Salvar: persiste a configuração sem reiniciar o serviço. */
static void on_frequency_save(GtkButton *button, gpointer user_data) {
    (void)button;
    save_frequency_config(user_data, FALSE);
}

/* Callback que salva e reinicia o daemon para aplicar imediatamente os novos limites. */
static void on_frequency_save_restart(GtkButton *button, gpointer user_data) {
    (void)button;
    save_frequency_config(user_data, TRUE);
}

/* Restaura valores padrão apenas na interface; o usuário ainda decide quando salvar. */
static void on_frequency_defaults(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *state = user_data;

    cpu_config_defaults(&state->config);
    if (state->have_frequency_range) {
        state->config.low_frequency_khz = state->min_khz;
        state->config.high_frequency_khz = state->max_khz;
    }
    frequency_config_to_widgets(state);
    set_status(state, "Padrões restaurados na tela; clique em salvar para aplicá-los.", FALSE);
}

/* Callback que força nova leitura dos limites fornecidos pelo kernel. */
static void on_hardware_refresh(GtkButton *button, gpointer user_data) {
    (void)button;
    refresh_hardware_detection(user_data, TRUE);
}

/* ------------------------------------------------------------------------- */
/* Runtime metrics and service status                                         */
/* ------------------------------------------------------------------------- */

/* Atualiza periodicamente uso total e frequência atual mostrados no topo da janela. */
static gboolean refresh_metrics(gpointer user_data) {
    AppState *state = user_data;

    CpuTimes current;
    char error[256] = {0};
    if (cpu_linux_read_times(&current, error, sizeof(error)) == 0) {
        if (state->have_previous_times) {
            double usage = cpu_linux_usage_percent(&state->previous_times, &current);
            char *usage_text = g_strdup_printf("Uso atual: %.1f%%", usage);
            gtk_label_set_text(GTK_LABEL(state->usage_label), usage_text);
            g_free(usage_text);
        }
        state->previous_times = current;
        state->have_previous_times = TRUE;
    }

    uint64_t frequency_khz = 0;
    if (cpu_linux_read_current_frequency(&frequency_khz, error, sizeof(error)) == 0) {
        char *frequency = format_frequency(frequency_khz);
        char *label = g_strdup_printf("Frequência atual: %s", frequency);
        gtk_label_set_text(GTK_LABEL(state->frequency_label), label);
        g_free(label);
        g_free(frequency);
    } else {
        gtk_label_set_text(GTK_LABEL(state->frequency_label), "Frequência atual: indisponível");
    }

    return G_SOURCE_CONTINUE;
}

/* Consulta o systemd periodicamente para mostrar o estado real do daemon. */
static gboolean refresh_service_status(gpointer user_data) {
    AppState *state = user_data;

    if (!frequency_service_is_installed()) {
        gtk_label_set_text(GTK_LABEL(state->service_label), "Serviço: não instalado");
        return G_SOURCE_CONTINUE;
    }

    char *service_state = command_systemctl_state("is-active", SERVICE_NAME);
    char *label = g_strdup_printf("Serviço: %s", service_state);
    gtk_label_set_text(GTK_LABEL(state->service_label), label);
    g_free(label);
    g_free(service_state);
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------------- */
/* Intel undervolt backend                                                    */
/* ------------------------------------------------------------------------- */

/* Copia offsets em mV para os controles correspondentes aos índices do backend. */
static void undervolt_values_to_widgets(
    AppState *state,
    const double offsets[UNDERVOLT_DOMAIN_COUNT]) {
    for (size_t index = 0; index < UNDERVOLT_DOMAIN_COUNT; ++index) {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->undervolt_spins[index]), offsets[index]);
    }
}

/* Coleta os offsets atuais da interface antes de validar e aplicar. */
static void undervolt_widgets_to_values(
    AppState *state,
    double offsets[UNDERVOLT_DOMAIN_COUNT]) {
    for (size_t index = 0; index < UNDERVOLT_DOMAIN_COUNT; ++index) {
        offsets[index] =
            gtk_spin_button_get_value(GTK_SPIN_BUTTON(state->undervolt_spins[index]));
    }
}

/* Mostra somente domínios retornados pelo hardware após a detecção; antes dela mantém todos visíveis. */
static void update_undervolt_domain_visibility(AppState *state) {
    for (size_t index = 0; index < UNDERVOLT_DOMAIN_COUNT; ++index) {
        gboolean visible =
            !state->undervolt_domains_detected || state->undervolt_present[index] != 0;
        gtk_widget_set_visible(state->undervolt_domain_labels[index], visible);
        gtk_widget_set_visible(state->undervolt_spins[index], visible);
        gtk_widget_set_visible(state->undervolt_domain_units[index], visible);
    }
}

/* Redetecta intel-undervolt, recarrega a configuração existente e atualiza disponibilidade dos controles. */
static void refresh_undervolt_backend(AppState *state) {
    state->undervolt_domains_detected = FALSE;
    memset(state->undervolt_present, 0, sizeof(state->undervolt_present));
    update_undervolt_domain_visibility(state);

    g_clear_pointer(&state->undervolt_program, g_free);
    state->undervolt_program = g_find_program_in_path("intel-undervolt");

    if (!cpu_linux_is_intel()) {
        gtk_label_set_text(GTK_LABEL(state->undervolt_backend_label),
                           "Indisponível: o processador detectado não é Intel.");
        gtk_widget_set_sensitive(state->undervolt_controls, FALSE);
        return;
    }

    if (state->undervolt_program == NULL) {
        gtk_label_set_text(GTK_LABEL(state->undervolt_backend_label),
                           "Indisponível: intel-undervolt não está instalado.");
        gtk_widget_set_sensitive(state->undervolt_controls, FALSE);
        return;
    }

    char *backend_text =
        g_strdup_printf("Backend: %s", state->undervolt_program);
    gtk_label_set_text(GTK_LABEL(state->undervolt_backend_label), backend_text);
    g_free(backend_text);
    gtk_widget_set_sensitive(state->undervolt_controls, TRUE);

    double offsets[UNDERVOLT_DOMAIN_COUNT];
    char error[256] = {0};
    if (undervolt_load_config(UNDERVOLT_CONFIG_PATH,
                              offsets,
                              error,
                              sizeof(error)) == 0) {
        undervolt_values_to_widgets(state, offsets);
    } else {
        set_status(state, error, TRUE);
    }

    gtk_check_button_set_active(GTK_CHECK_BUTTON(state->undervolt_boot_check),
                                command_systemctl_is_enabled(UNDERVOLT_SERVICE_NAME));
}

/* Executa a leitura privilegiada do backend, atualiza valores e registra quais domínios existem na CPU. */
static void on_undervolt_read(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *state = user_data;

    if (state->undervolt_program == NULL) {
        set_status(state, "intel-undervolt não está disponível.", TRUE);
        return;
    }

    /*
     * Reading the voltage mailbox requires the same privileged MSR access as
     * applying an offset, so this one diagnostic command is also run by
     * pkexec. The graphical process itself remains unprivileged.
     */
    char *argv[] = {
        (char *)"pkexec",
        state->undervolt_program,
        (char *)"read",
        NULL,
    };
    CommandResult result;
    if (!command_run(argv, &result)) {
        set_status(state, command_failure_detail(&result), TRUE);
        command_result_clear(&result);
        return;
    }

    double offsets[UNDERVOLT_DOMAIN_COUNT];
    unsigned char present[UNDERVOLT_DOMAIN_COUNT];
    undervolt_defaults(offsets);
    size_t found =
        undervolt_parse_read_output_masked(result.stdout_text, offsets, present);
    if (found == 0) {
        set_status(state,
                   "O backend respondeu, mas nenhum domínio de tensão pôde ser interpretado.",
                   TRUE);
    } else {
        memcpy(state->undervolt_present, present, sizeof(state->undervolt_present));
        state->undervolt_domains_detected = TRUE;
        update_undervolt_domain_visibility(state);
        undervolt_values_to_widgets(state, offsets);

        char *message = g_strdup_printf(
            "Hardware detectado: %zu domínio(s) de tensão suportado(s). "
            "Domínios ausentes foram ocultados e não serão alterados.",
            found);
        set_status(state, message, FALSE);
        g_free(message);
    }

    command_result_clear(&result);
}

/* Preserva a configuração existente, altera somente domínios detectados, aplica os offsets e sincroniza a opção de boot. */
static gboolean write_and_apply_undervolt(AppState *state) {
    double offsets[UNDERVOLT_DOMAIN_COUNT];
    undervolt_widgets_to_values(state, offsets);

    char error[256] = {0};
    if (undervolt_validate(offsets, error, sizeof(error)) != 0) {
        set_status(state, error, TRUE);
        return FALSE;
    }

    char *temporary_path = NULL;
    GError *temporary_error = NULL;
    int fd = g_file_open_tmp("intel-undervolt-XXXXXX", &temporary_path, &temporary_error);
    if (fd < 0) {
        set_status(state,
                   temporary_error != NULL ? temporary_error->message
                                           : "Não foi possível criar arquivo temporário.",
                   TRUE);
        g_clear_error(&temporary_error);
        return FALSE;
    }
    close(fd);

    /* Preserve every non-voltage setting already present in the backend file. */
    const unsigned char *present =
        state->undervolt_domains_detected ? state->undervolt_present : NULL;
    if (undervolt_write_config_copy_masked(UNDERVOLT_CONFIG_PATH,
                                           temporary_path,
                                           offsets,
                                           present,
                                           error,
                                           sizeof(error)) != 0) {
        set_status(state, error, TRUE);
        g_unlink(temporary_path);
        g_free(temporary_path);
        return FALSE;
    }

    char *install_argv[] = {
        (char *)"install",
        (char *)"-m",
        (char *)"0644",
        temporary_path,
        (char *)UNDERVOLT_CONFIG_PATH,
        NULL,
    };
    char *detail = NULL;
    gboolean installed = command_run_pkexec(install_argv, &detail);
    g_unlink(temporary_path);
    g_free(temporary_path);

    if (!installed) {
        set_status(state, detail, TRUE);
        g_free(detail);
        return FALSE;
    }
    g_free(detail);

    /* intel-undervolt itself performs the MSR write and reports firmware locks. */
    char *apply_argv[] = {
        state->undervolt_program,
        (char *)"apply",
        NULL,
    };
    if (!command_run_pkexec(apply_argv, &detail)) {
        set_status(state, detail, TRUE);
        g_free(detail);
        return FALSE;
    }
    g_free(detail);

    /* Match systemd persistence to the checkbox visible to the user. */
    gboolean at_boot =
        gtk_check_button_get_active(GTK_CHECK_BUTTON(state->undervolt_boot_check));
    char *service_argv_enable[] = {
        (char *)"systemctl",
        (char *)"enable",
        (char *)"--now",
        (char *)UNDERVOLT_SERVICE_NAME,
        NULL,
    };
    char *service_argv_disable[] = {
        (char *)"systemctl",
        (char *)"disable",
        (char *)UNDERVOLT_SERVICE_NAME,
        NULL,
    };
    char **service_argv = at_boot ? service_argv_enable : service_argv_disable;

    if (!command_run_pkexec(service_argv, &detail)) {
        char *message = g_strdup_printf(
            "Undervolt aplicado, mas não foi possível alterar o serviço de boot: %s",
            detail != NULL ? detail : "erro desconhecido");
        set_status(state, message, TRUE);
        g_free(message);
        g_free(detail);
        return TRUE; /* Voltage was applied even though persistence failed. */
    }
    g_free(detail);

    set_status(state,
               at_boot ? "Undervolt aplicado e configurado para o boot."
                       : "Undervolt aplicado; reaplicação automática no boot desativada.",
               FALSE);
    return TRUE;
}

/* Callback do botão Salvar e aplicar; delega todo o fluxo validado à função central. */
static void on_undervolt_apply(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *state = user_data;

    if (state->undervolt_program == NULL) {
        set_status(state, "intel-undervolt não está disponível.", TRUE);
        return;
    }
    write_and_apply_undervolt(state);
}

/* Coloca os campos visíveis em 0 mV sem aplicar automaticamente, evitando mudanças acidentais. */
static void on_undervolt_zero(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *state = user_data;
    double offsets[UNDERVOLT_DOMAIN_COUNT];
    undervolt_defaults(offsets);
    undervolt_values_to_widgets(state, offsets);
    set_status(state, "Offsets colocados em 0 mV na tela; clique em aplicar para efetivar.", FALSE);
}

/* Callback que refaz apenas a detecção do backend de tensão. */
static void on_undervolt_refresh(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *state = user_data;
    refresh_undervolt_backend(state);
    set_status(state, "Detecção do backend de tensão atualizada.", FALSE);
}

/* ------------------------------------------------------------------------- */
/* Page construction                                                          */
/* ------------------------------------------------------------------------- */

/* Monta todos os widgets da aba Frequência e conecta seus callbacks. */
static GtkWidget *build_frequency_page(AppState *state) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(page, 16);
    gtk_widget_set_margin_bottom(page, 16);
    gtk_widget_set_margin_start(page, 16);
    gtk_widget_set_margin_end(page, 16);

    GtkWidget *description = new_left_label(
        "Alterna automaticamente entre dois clocks conforme o uso total da CPU.");
    gtk_label_set_wrap(GTK_LABEL(description), TRUE);
    gtk_box_append(GTK_BOX(page), description);

    state->service_label = new_left_label("Serviço: --");
    state->usage_label = new_left_label("Uso atual: --");
    state->frequency_label = new_left_label("Frequência atual: --");
    state->range_label = new_left_label("Faixa comum: --");
    state->policy_label = new_left_label("Políticas: --");

    GtkWidget *metrics = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_append(GTK_BOX(metrics), state->service_label);
    gtk_box_append(GTK_BOX(metrics), state->usage_label);
    gtk_box_append(GTK_BOX(metrics), state->frequency_label);
    gtk_box_append(GTK_BOX(metrics), state->range_label);
    gtk_box_append(GTK_BOX(metrics), state->policy_label);
    gtk_box_append(GTK_BOX(page), metrics);

    GtkWidget *frame = gtk_frame_new("Histerese e frequências");
    state->frequency_controls = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(state->frequency_controls), 10);
    gtk_grid_set_column_spacing(GTK_GRID(state->frequency_controls), 10);
    gtk_widget_set_margin_top(state->frequency_controls, 12);
    gtk_widget_set_margin_bottom(state->frequency_controls, 12);
    gtk_widget_set_margin_start(state->frequency_controls, 12);
    gtk_widget_set_margin_end(state->frequency_controls, 12);

    state->low_threshold_spin = new_spin(0.0, 99.0, 1.0, 0);
    state->high_threshold_spin = new_spin(1.0, 100.0, 1.0, 0);
    state->low_frequency_spin = new_spin(100.0, 10000.0, 100.0, 0);
    state->high_frequency_spin = new_spin(100.0, 10000.0, 100.0, 0);
    state->interval_spin = new_spin(100.0, 5000.0, 100.0, 0);

    grid_add_row(state->frequency_controls,
                 0,
                 "Baixa carga abaixo de",
                 state->low_threshold_spin,
                 "%");
    grid_add_row(state->frequency_controls,
                 1,
                 "Clock de baixa carga",
                 state->low_frequency_spin,
                 "MHz");
    grid_add_row(state->frequency_controls,
                 2,
                 "Alta carga acima de",
                 state->high_threshold_spin,
                 "%");
    grid_add_row(state->frequency_controls,
                 3,
                 "Clock de alta carga",
                 state->high_frequency_spin,
                 "MHz");
    grid_add_row(state->frequency_controls,
                 4,
                 "Intervalo de leitura",
                 state->interval_spin,
                 "ms");

    gtk_frame_set_child(GTK_FRAME(frame), state->frequency_controls);
    gtk_box_append(GTK_BOX(page), frame);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *save = gtk_button_new_with_label("Salvar");
    GtkWidget *save_restart = gtk_button_new_with_label("Salvar e reiniciar serviço");
    GtkWidget *defaults = gtk_button_new_with_label("Restaurar padrões");
    GtkWidget *refresh = gtk_button_new_with_label("Redetectar CPU");
    g_signal_connect(save, "clicked", G_CALLBACK(on_frequency_save), state);
    g_signal_connect(save_restart, "clicked", G_CALLBACK(on_frequency_save_restart), state);
    g_signal_connect(defaults, "clicked", G_CALLBACK(on_frequency_defaults), state);
    g_signal_connect(refresh, "clicked", G_CALLBACK(on_hardware_refresh), state);
    gtk_box_append(GTK_BOX(buttons), save);
    gtk_box_append(GTK_BOX(buttons), save_restart);
    gtk_box_append(GTK_BOX(buttons), defaults);
    gtk_box_append(GTK_BOX(buttons), refresh);
    gtk_box_append(GTK_BOX(page), buttons);

    return page;
}

/* Monta a aba Tensão; as linhas de domínio são guardadas separadamente para poder ocultá-las dinamicamente. */
static GtkWidget *build_undervolt_page(AppState *state) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(page, 16);
    gtk_widget_set_margin_bottom(page, 16);
    gtk_widget_set_margin_start(page, 16);
    gtk_widget_set_margin_end(page, 16);

    GtkWidget *description = new_left_label(
        "Controle de offset de tensão para CPUs Intel compatíveis, usando intel-undervolt.");
    gtk_label_set_wrap(GTK_LABEL(description), TRUE);
    gtk_box_append(GTK_BOX(page), description);

    GtkWidget *warning = new_left_label(
        "Use valores negativos pequenos e teste estabilidade. Algumas BIOS/CPUs bloqueiam "
        "undervolt; nesse caso o backend recusará a aplicação.");
    gtk_label_set_wrap(GTK_LABEL(warning), TRUE);
    gtk_widget_add_css_class(warning, "warning");
    gtk_box_append(GTK_BOX(page), warning);

    state->undervolt_backend_label = new_left_label("Backend: detectando...");
    gtk_box_append(GTK_BOX(page), state->undervolt_backend_label);

    GtkWidget *frame = gtk_frame_new("Offsets de tensão");
    state->undervolt_controls = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(state->undervolt_controls, 12);
    gtk_widget_set_margin_bottom(state->undervolt_controls, 12);
    gtk_widget_set_margin_start(state->undervolt_controls, 12);
    gtk_widget_set_margin_end(state->undervolt_controls, 12);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);

    for (size_t index = 0; index < UNDERVOLT_DOMAIN_COUNT; ++index) {
        state->undervolt_domain_labels[index] =
            new_left_label(UNDERVOLT_DOMAIN_NAMES[index]);
        state->undervolt_spins[index] =
            new_spin(UNDERVOLT_MIN_MV, UNDERVOLT_MAX_MV, 1.0, 2);
        state->undervolt_domain_units[index] = new_left_label("mV");

        gtk_grid_attach(GTK_GRID(grid),
                        state->undervolt_domain_labels[index],
                        0,
                        (int)index,
                        1,
                        1);
        gtk_grid_attach(GTK_GRID(grid),
                        state->undervolt_spins[index],
                        1,
                        (int)index,
                        1,
                        1);
        gtk_grid_attach(GTK_GRID(grid),
                        state->undervolt_domain_units[index],
                        2,
                        (int)index,
                        1,
                        1);
    }
    gtk_box_append(GTK_BOX(state->undervolt_controls), grid);

    state->undervolt_boot_check =
        gtk_check_button_new_with_label("Reaplicar automaticamente no boot");
    gtk_box_append(GTK_BOX(state->undervolt_controls), state->undervolt_boot_check);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *read = gtk_button_new_with_label("Ler hardware");
    GtkWidget *apply = gtk_button_new_with_label("Salvar e aplicar");
    GtkWidget *zero = gtk_button_new_with_label("Colocar tudo em 0 mV");
    GtkWidget *refresh = gtk_button_new_with_label("Redetectar backend");
    g_signal_connect(read, "clicked", G_CALLBACK(on_undervolt_read), state);
    g_signal_connect(apply, "clicked", G_CALLBACK(on_undervolt_apply), state);
    g_signal_connect(zero, "clicked", G_CALLBACK(on_undervolt_zero), state);
    g_signal_connect(refresh, "clicked", G_CALLBACK(on_undervolt_refresh), state);
    gtk_box_append(GTK_BOX(buttons), read);
    gtk_box_append(GTK_BOX(buttons), apply);
    gtk_box_append(GTK_BOX(buttons), zero);
    gtk_box_append(GTK_BOX(buttons), refresh);
    gtk_box_append(GTK_BOX(state->undervolt_controls), buttons);

    gtk_frame_set_child(GTK_FRAME(frame), state->undervolt_controls);
    gtk_box_append(GTK_BOX(page), frame);

    GtkWidget *backend_note = new_left_label(
        "O programa nunca envia offset positivo pela interface. Alterações em /etc são feitas "
        "por pkexec; a GUI continua executando como usuário comum.");
    gtk_label_set_wrap(GTK_LABEL(backend_note), TRUE);
    gtk_box_append(GTK_BOX(page), backend_note);

    return page;
}

/* ------------------------------------------------------------------------- */
/* Application lifecycle                                                      */
/* ------------------------------------------------------------------------- */

/* Libera timers e memória pertencentes ao estado quando a janela é destruída. */
static void app_state_free(gpointer data) {
    AppState *state = data;
    if (state == NULL) {
        return;
    }

    if (state->metrics_timer != 0) {
        g_source_remove(state->metrics_timer);
    }
    if (state->service_timer != 0) {
        g_source_remove(state->service_timer);
    }
    g_free(state->undervolt_program);
    g_free(state);
}

/* Cria o estado da aplicação, monta a janela, carrega configuração e inicia atualizações periódicas. */
static void activate(GtkApplication *application, gpointer user_data) {
    (void)user_data;

    AppState *state = g_new0(AppState, 1);

    char config_error[256] = {0};
    if (cpu_config_load(CONFIG_PATH,
                        &state->config,
                        config_error,
                        sizeof(config_error)) != 0) {
        cpu_config_defaults(&state->config);
    }

    state->window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(state->window), "CPU Switch Control");
    gtk_window_set_default_size(GTK_WINDOW(state->window), 820, 720);
    g_object_set_data_full(G_OBJECT(state->window), "cpu-switch-state", state, app_state_free);

    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_window_set_child(GTK_WINDOW(state->window), scroller);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(root, 16);
    gtk_widget_set_margin_bottom(root, 16);
    gtk_widget_set_margin_start(root, 16);
    gtk_widget_set_margin_end(root, 16);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), root);

    GtkWidget *title = new_left_label("CPU Switch Control");
    gtk_widget_add_css_class(title, "title-1");
    gtk_box_append(GTK_BOX(root), title);

    state->status_label = new_left_label("");
    gtk_label_set_wrap(GTK_LABEL(state->status_label), TRUE);
    gtk_box_append(GTK_BOX(root), state->status_label);

    GtkWidget *notebook = gtk_notebook_new();
    gtk_widget_set_vexpand(notebook, TRUE);
    GtkWidget *frequency_page = build_frequency_page(state);
    GtkWidget *undervolt_page = build_undervolt_page(state);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), frequency_page, gtk_label_new("Frequência"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), undervolt_page, gtk_label_new("Tensão"));
    gtk_box_append(GTK_BOX(root), notebook);

    refresh_hardware_detection(state, FALSE);
    refresh_undervolt_backend(state);

    if (config_error[0] != '\0') {
        set_status(state, config_error, TRUE);
    }

    refresh_metrics(state);
    refresh_service_status(state);
    state->metrics_timer = g_timeout_add(500, refresh_metrics, state);
    state->service_timer = g_timeout_add_seconds(2, refresh_service_status, state);

    gtk_window_present(GTK_WINDOW(state->window));
}

/* Inicializa GtkApplication e entrega o ciclo de eventos ao GTK. */
int main(int argc, char **argv) {
    GtkApplication *application =
        gtk_application_new("io.github.xoykor.cpu-switch-control", (GApplicationFlags)0);
    g_signal_connect(application, "activate", G_CALLBACK(activate), NULL);

    int status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return status;
}
