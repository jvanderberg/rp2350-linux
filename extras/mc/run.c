/*
 * run.c - run child programs with the terminal handed over. See run.h.
 */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "run.h"
#include "screen.h"

/* Wait for one key press on the normal screen, then swallow the rest of
 * its escape sequence (if any). */
static void wait_key(void)
{
    struct termios saved, t;
    struct pollfd pfd;
    char buf[32];

    if (tcgetattr(STDIN_FILENO, &saved) != 0)
        return;
    t = saved;
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    if (read(STDIN_FILENO, buf, 1) == 1) {
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        while (poll(&pfd, 1, 50) > 0 && read(STDIN_FILENO, buf, sizeof buf) > 0)
            ;
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &saved);
}

static void press_any_key(void)
{
    fputs("\nPress any key to continue...", stdout);
    fflush(stdout);
    wait_key();
    fputs("\n", stdout);
    fflush(stdout);
}

/* fork + exec + wait. While the child runs we ignore ^C and ^\ (they are
 * meant for the child); the child gets the default handlers back. */
static void spawn(const char *dir, char *const argv[])
{
    struct sigaction ign, old_int, old_quit;
    pid_t pid;
    int status;

    memset(&ign, 0, sizeof ign);
    ign.sa_handler = SIG_IGN;
    sigaction(SIGINT, &ign, &old_int);
    sigaction(SIGQUIT, &ign, &old_quit);

    pid = fork();
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        if (chdir(dir) != 0) {
            fprintf(stderr, "cd %s: %s\n", dir, strerror(errno));
            _exit(126);
        }
        execvp(argv[0], argv);
        fprintf(stderr, "%s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    if (pid < 0)
        fprintf(stderr, "fork: %s\n", strerror(errno));
    else
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            ;

    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGQUIT, &old_quit, NULL);
}

void run_argv(const char *dir, char *const argv[], int pause)
{
    scr_suspend();
    spawn(dir, argv);
    if (pause)
        press_any_key();
    scr_resume();
}

void run_command(const char *dir, const char *cmd)
{
    char *argv[4];

    argv[0] = "/bin/sh";
    argv[1] = "-c";
    argv[2] = (char *)cmd;
    argv[3] = NULL;
    scr_suspend();
    printf("%s %s\n", geteuid() == 0 ? "#" : "$", cmd);
    fflush(stdout);
    spawn(dir, argv);
    press_any_key();
    scr_resume();
}

void run_script(const char *dir, const char *script)
{
    char *argv[4];

    argv[0] = "/bin/sh";
    argv[1] = "-c";
    argv[2] = (char *)script;
    argv[3] = NULL;
    scr_suspend();
    spawn(dir, argv);
    press_any_key();
    scr_resume();
}

void run_with(const char *dir, const char *var, const char *fallback,
              const char *file)
{
    const char *prog = getenv(var);
    char script[256];
    char *argv[6];

    if (prog == NULL || *prog == '\0')
        prog = fallback;
    /* Let the shell split "$PROG" into program + options; the file name
     * is passed as $1 so it needs no quoting. */
    snprintf(script, sizeof script, "exec %s \"$1\"", prog);
    argv[0] = "/bin/sh";
    argv[1] = "-c";
    argv[2] = script;
    argv[3] = "sh";
    argv[4] = (char *)file;
    argv[5] = NULL;
    run_argv(dir, argv, 0);
}

void run_show_output(void)
{
    scr_suspend();
    wait_key();
    scr_resume();
}
