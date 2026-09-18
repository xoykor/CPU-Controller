#ifndef CPU_SWITCH_COMMAND_H
#define CPU_SWITCH_COMMAND_H

#include <glib.h>

/* Captured result of a short-lived child process. */
typedef struct {
    gboolean spawned;
    int exit_status;
    char *stdout_text;
    char *stderr_text;
} CommandResult;

/* Run ARGV synchronously and capture stdout/stderr. */
gboolean command_run(char *const argv[], CommandResult *result);

/* Release strings owned by RESULT and reset it to zero. */
void command_result_clear(CommandResult *result);

/*
 * Run a command through pkexec. DETAIL receives a user-facing explanation on
 * failure and must be freed with g_free().
 */
gboolean command_run_pkexec(char *const argv[], char **detail);

/* Return a newly allocated textual state from systemctl, e.g. "active". */
char *command_systemctl_state(const char *verb, const char *unit);

/* True only when systemctl reports UNIT as enabled. */
gboolean command_systemctl_is_enabled(const char *unit);

#endif
