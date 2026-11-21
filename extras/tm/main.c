/*
 * main.c - tm, a small terminal multiplexer.
 *
 *   tm                    attach to session "main", starting it if needed
 *   tm new [-d] [-s NAME] start a new session and attach (-d: don't)
 *   tm attach [-t NAME]   attach (also: tm a)
 *   tm ls                 list sessions
 *   tm kill [-t NAME]     end a session and everything in it
 *
 * Each session is a background server listening on /tmp/tm-UID/NAME.
 * Scrollback goes to a ring file in $TM_HISTORY_DIR (default /mnt/sd/.tm,
 * if it can be created), $TM_HISTORY_KB per window (default 2048); with
 * no usable directory, 64 KB per window is kept in RAM. History is
 * compressed either way. TM_MOUSE=0 starts with tm's mouse handling off
 * (Ctrl-B m toggles it).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "client.h"
#include "proto.h"
#include "server.h"

#define DEFAULT_SESSION "main"
#define DEFAULT_HISTORY_DIR "/mnt/sd/.tm"

static char sock_dir[64];

static void usage(void)
{
    fputs("usage: tm [new [-d] [-s NAME] | attach [-t NAME] | ls | kill [-t NAME]]\n"
          "Inside tm, press Ctrl-B ? for the keys.\n", stderr);
    exit(2);
}

static void die(const char *what)
{
    fprintf(stderr, "tm: %s: %s\n", what, strerror(errno));
    exit(1);
}

static void make_sock_dir(void)
{
    struct stat st;

    snprintf(sock_dir, sizeof sock_dir, "/tmp/tm-%ld", (long)getuid());
    if (mkdir(sock_dir, 0700) != 0 && errno != EEXIST)
        die(sock_dir);
    if (stat(sock_dir, &st) != 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != getuid()) {
        fprintf(stderr, "tm: %s is not my directory\n", sock_dir);
        exit(1);
    }
    chmod(sock_dir, 0700);
}

static int valid_name(const char *name)
{
    size_t n = strlen(name);

    return n > 0 && n < 32 && strchr(name, '/') == NULL && name[0] != '.';
}

static void sock_addr(struct sockaddr_un *a, const char *name)
{
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    snprintf(a->sun_path, sizeof a->sun_path, "%s/%s", sock_dir, name);
}

/* Connect to a session: the socket, or -1 if it is not running (a stale
 * socket left by a crash is removed). */
static int connect_session(const char *name)
{
    struct sockaddr_un a;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0)
        die("socket");
    sock_addr(&a, name);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0)
        return fd;
    if (errno == ECONNREFUSED)
        unlink(a.sun_path);
    close(fd);
    return -1;
}

static size_t env_kb(const char *var, size_t def)
{
    const char *s = getenv(var);

    return s != NULL && atol(s) > 0 ? (size_t)atol(s) * 1024 : def;
}

/* Where scrollback goes: a directory we can write, or NULL for RAM. */
static const char *history_dir(void)
{
    const char *d = getenv("TM_HISTORY_DIR");

    if (d == NULL)
        d = DEFAULT_HISTORY_DIR;
    if (*d == '\0' || strcmp(d, "none") == 0)
        return NULL;
    mkdir(d, 0700);
    return access(d, W_OK) == 0 ? d : NULL;
}

/* Start a session's server in the background and attach to it (unless
 * detached). */
static int new_session(const char *name, int detached)
{
    static struct server_config cfg;
    static char path[sizeof ((struct sockaddr_un *)0)->sun_path];
    struct sockaddr_un a;
    struct winsize ws;
    int lfd, fd, rows = 24, cols = 80;
    pid_t pid;

    if ((fd = connect_session(name)) >= 0) {
        close(fd);
        fprintf(stderr, "tm: session %s already exists\n", name);
        return 1;
    }
    if (ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        rows = ws.ws_row;
        cols = ws.ws_col;
    }
    sock_addr(&a, name);
    snprintf(path, sizeof path, "%s", a.sun_path);
    lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0)
        die("socket");
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) != 0)
        die(path);
    if (listen(lfd, 4) != 0)
        die("listen");
    fcntl(lfd, F_SETFD, FD_CLOEXEC);

    cfg.session = name;
    cfg.history_dir = history_dir();
    cfg.history_disk = env_kb("TM_HISTORY_KB", 2048 * 1024);
    cfg.history_ram = 64 * 1024;

    pid = fork();
    if (pid < 0)
        die("fork");
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);

        setsid();
        if (nul >= 0) {
            dup2(nul, 0);
            dup2(nul, 1);
            dup2(nul, 2);
            if (nul > 2)
                close(nul);
        }
        server_run(lfd, path, &cfg, rows, cols);
        _exit(0);
    }
    close(lfd);
    if (detached)
        return 0;
    fd = connect_session(name);     /* queued by listen() already */
    if (fd < 0)
        die("connect");
    return client_run(fd, name);
}

static int attach(const char *name)
{
    int fd = connect_session(name);

    if (fd < 0) {
        fprintf(stderr, "tm: no session %s\n", name);
        return 1;
    }
    return client_run(fd, name);
}

static int list(void)
{
    DIR *d = opendir(sock_dir);
    struct dirent *e;
    int found = 0;

    if (d == NULL)
        die(sock_dir);
    while ((e = readdir(d)) != NULL) {
        int fd;

        if (e->d_name[0] == '.' || (fd = connect_session(e->d_name)) < 0)
            continue;
        close(fd);
        printf("%s\n", e->d_name);
        found = 1;
    }
    closedir(d);
    if (!found)
        fputs("no sessions\n", stderr);
    return found ? 0 : 1;
}

static int kill_session(const char *name)
{
    int fd = connect_session(name);
    char c;

    if (fd < 0) {
        fprintf(stderr, "tm: no session %s\n", name);
        return 1;
    }
    msg_send(fd, MSG_EXIT, "", 0);
    while (read(fd, &c, 1) > 0)     /* wait until it is gone */
        ;
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "", *name = DEFAULT_SESSION;
    int i, detached = 0;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0)
            detached = 1;
        else if ((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "-t") == 0) &&
            i + 1 < argc)
            name = argv[++i];
        else
            usage();
    }
    if (!valid_name(name)) {
        fprintf(stderr, "tm: bad session name: %s\n", name);
        return 2;
    }
    make_sock_dir();

    if (strcmp(cmd, "ls") == 0 || strcmp(cmd, "list") == 0)
        return list();
    if (strcmp(cmd, "kill") == 0)
        return kill_session(name);
    if (strcmp(cmd, "") != 0 && strcmp(cmd, "new") != 0 &&
        strcmp(cmd, "a") != 0 && strcmp(cmd, "attach") != 0)
        usage();

    if (strcmp(cmd, "new") == 0 && detached)
        return new_session(name, 1);
    if (getenv("TM") != NULL) {
        fputs("tm: already inside tm (unset TM to force)\n", stderr);
        return 1;
    }
    if (!isatty(0)) {
        fputs("tm: not a terminal\n", stderr);
        return 1;
    }
    if (strcmp(cmd, "new") == 0)
        return new_session(name, 0);
    if (strcmp(cmd, "a") == 0 || strcmp(cmd, "attach") == 0)
        return attach(name);
    {
        int fd = connect_session(name);

        return fd >= 0 ? client_run(fd, name) : new_session(name, 0);
    }
}
