/*
 * CPU Switch Control - command.c
 *
 * Camada responsável por executar processos externos usados pela interface.
 * A GUI permanece sem privilégios; quando uma ação administrativa é necessária,
 * este módulo monta a chamada via pkexec e devolve stdout, stderr e status de
 * saída de forma previsível para a camada gráfica.
 */

#include "command.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Process execution                                                          */
/* ------------------------------------------------------------------------- */

/* Libera as strings capturadas de um comando e zera o resultado para evitar reuso acidental. */
void command_result_clear(CommandResult *result) {
    if (result == NULL) {
        return;
    }

    g_clear_pointer(&result->stdout_text, g_free);
    g_clear_pointer(&result->stderr_text, g_free);
    result->spawned = FALSE;
    result->exit_status = 1;
}

/* Executa um processo de forma síncrona, capturando stdout/stderr e traduzindo o wait status para sucesso ou falha. */
gboolean command_run(char *const argv[], CommandResult *result) {
    if (result == NULL || argv == NULL || argv[0] == NULL) {
        return FALSE;
    }

    *result = (CommandResult){0};
    result->exit_status = 1;

    GError *error = NULL;
    int wait_status = 0;
    result->spawned = g_spawn_sync(NULL,
                                   (char **)argv,
                                   NULL,
                                   G_SPAWN_SEARCH_PATH,
                                   NULL,
                                   NULL,
                                   &result->stdout_text,
                                   &result->stderr_text,
                                   &wait_status,
                                   &error);
    if (!result->spawned) {
        if (result->stderr_text == NULL && error != NULL) {
            result->stderr_text = g_strdup(error->message);
        }
        g_clear_error(&error);
        return FALSE;
    }

    /*
     * GLib keeps the platform wait status separate from process spawning.
     * Convert it to the simple 0/non-zero convention the UI needs.
     */
    GError *status_error = NULL;
    if (g_spawn_check_wait_status(wait_status, &status_error)) {
        result->exit_status = 0;
    } else {
        result->exit_status = 1;
        if (result->stderr_text == NULL && status_error != NULL) {
            result->stderr_text = g_strdup(status_error->message);
        }
        g_clear_error(&status_error);
    }

    return result->exit_status == 0;
}

/* ------------------------------------------------------------------------- */
/* Privileged commands                                                        */
/* ------------------------------------------------------------------------- */

/* Prefixa um comando administrativo com pkexec sem elevar o processo gráfico inteiro. */
gboolean command_run_pkexec(char *const argv[], char **detail) {
    if (detail != NULL) {
        *detail = NULL;
    }

    /* Prepend pkexec without forcing callers to build a second argument list. */
    GPtrArray *arguments = g_ptr_array_new();
    g_ptr_array_add(arguments, (gpointer)"pkexec");
    for (size_t index = 0; argv[index] != NULL; ++index) {
        g_ptr_array_add(arguments, argv[index]);
    }
    g_ptr_array_add(arguments, NULL);

    CommandResult result;
    gboolean ok = command_run((char *const *)arguments->pdata, &result);
    g_ptr_array_free(arguments, TRUE);

    if (!ok && detail != NULL) {
        const char *message = result.stderr_text;
        if (message == NULL || *g_strstrip(result.stderr_text) == '\0') {
            message = "A autorização administrativa foi cancelada ou o comando falhou.";
        }
        *detail = g_strdup(message);
    }

    command_result_clear(&result);
    return ok;
}

/* ------------------------------------------------------------------------- */
/* systemd helpers                                                            */
/* ------------------------------------------------------------------------- */

char *command_systemctl_state(const char *verb, const char *unit) {
    char *argv[] = {(char *)"systemctl", (char *)verb, (char *)unit, NULL};
    CommandResult result;
    gboolean ok = command_run(argv, &result);

    char *state = NULL;
    if (result.stdout_text != NULL) {
        g_strstrip(result.stdout_text);
    }

    if (result.stdout_text != NULL && result.stdout_text[0] != '\0') {
        state = g_strdup(result.stdout_text);
    } else {
        state = g_strdup(ok ? "ok" : "indisponível");
    }

    command_result_clear(&result);
    return state;
}

/* Consulta systemd para refletir na interface se um serviço está habilitado no boot. */
gboolean command_systemctl_is_enabled(const char *unit) {
    char *state = command_systemctl_state("is-enabled", unit);
    gboolean enabled = g_strcmp0(state, "enabled") == 0;
    g_free(state);
    return enabled;
}
