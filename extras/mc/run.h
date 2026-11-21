/*
 * run.h - running other programs (viewer, editor, typed commands).
 *
 * The screen is handed back to the terminal (normal screen, cooked mode)
 * while the child runs, then retaken with a full repaint. Output of
 * commands stays on the normal screen, where Ctrl-O shows it again.
 */
#ifndef RUN_H
#define RUN_H

/* Run argv[0] with arguments in directory `dir`. With `pause`, wait for
 * a key afterwards so the output can be read. */
void run_argv(const char *dir, char *const argv[], int pause);

/* Run a shell command line in `dir`, echoing it like a shell would, and
 * wait for a key afterwards. */
void run_command(const char *dir, const char *cmd);

/* Run a (possibly multi-line) shell script in `dir` without echoing it,
 * and wait for a key afterwards. Used by the user menu. */
void run_script(const char *dir, const char *script);

/* Open `file` (in `dir`) with the program named by environment variable
 * `var` ("PAGER", "EDITOR"), or `fallback` if it is unset. The variable
 * may include arguments, e.g. EDITOR="nano -w". */
void run_with(const char *dir, const char *var, const char *fallback,
              const char *file);

/* Ctrl-O: show the normal screen (command output) until a key is hit. */
void run_show_output(void);

#endif /* RUN_H */
