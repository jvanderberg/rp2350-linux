/*
 * server.c - the tm server. See server.h.
 *
 * One poll() loop watches the listening socket, the attached client and
 * every window's pty. Program output goes through the window's emulator;
 * keystrokes from the client go to the active window (or to copy mode, a
 * prompt, or a prefix command). After each round, if anything changed,
 * the screen is composed and the difference sent to the client, at most
 * about 60 times a second so a flood of output is drawn in batches.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "copy.h"
#include "keys.h"
#include "proto.h"
#include "render.h"
#include "server.h"
#include "util.h"
#include "window.h"

#define PREFIX      K_CTRL('b')
/* Output is drawn at most this often while it streams. The first frame
 * after a quiet spell (a keystroke's echo, typically) goes out at once. */
#define FRAME_MS    25
#define A_STATUS    MKATTR(0, 2, 0) /* black on green, tmux's default */
#define A_MESSAGE   MKATTR(0, 3, 0) /* black on yellow */

enum { P_NONE, P_RENAME, P_KILL, P_SEARCH_DOWN, P_SEARCH_UP };
enum { VIEW_WINDOW, VIEW_COPY, VIEW_HELP };

static const char *const help_text[] = {
    "tm keys - press the prefix Ctrl-B, then:",
    "",
    "  c      new window              d      detach",
    "  n  p   next / previous window  0-9    go to window",
    "  l      last window             ,      rename window",
    "  &  x   kill window             r      redraw",
    "  [      copy mode (PgUp too)    ]      paste",
    "  ?      this help               C-b    send Ctrl-B",
    "  m      mouse on / off",
    "",
    "Copy mode: arrows or hjkl, PgUp/PgDn, g/G top/bottom, 0/$,",
    "/ and ? search down/up, n/N again, Space starts a selection,",
    "Enter copies it, q leaves.",
    "",
    "Mouse: click a window name to switch; the wheel scrolls back;",
    "drag to select and copy (Shift-drag uses the terminal's own",
    "selection). Programs that use the mouse get it themselves.",
    "",
    "Press any key to close this help.",
};

static struct {
    const struct server_config *cfg;
    const char *socket_path;
    int lfd, cfd;
    unsigned char in[16384];        /* partial frames from the client */
    size_t inlen;
    int fresh;                      /* client just attached */
    int rows, cols;                 /* client size; 0 until it says */
    int def_rows, def_cols;         /* size for windows when detached */
    struct term term;
    struct cell *scr;               /* the composed frame */
    unsigned char *mask;            /* rows of scr composed this frame */
    int full;                       /* next frame: compose every row */
    struct window *shown;           /* what the last frame showed ... */
    int shown_mode;                 /* ... and how (VIEW_*) */
    int redraw;                     /* tm's own state changed (a prompt,
                                     * copy mode, ...): draw a frame */
    struct buf out;

    struct window *windows, *cur, *last;
    int prefix;                     /* prefix key seen */
    int prompt;                     /* P_*: status-line prompt */
    char pbuf[64];
    char msg[96];                   /* status-line message ... */
    long long msg_until;            /* ... shown until then */
    int help;
    char *paste;
    size_t paste_len;
    char host[64];
    int mouse;                      /* we handle the mouse (C-b m) */
    int tab_x0[16], tab_x1[16], tab_id[16], ntabs; /* status-line names */
    int dragging;                   /* a left-button drag selects */
    int press_x, press_y, pressed;  /* where the left button went down */
    unsigned char kpend[32];        /* a mouse report cut off mid-way */
    size_t kpend_len;
    long long last_frame, last_names;
    int last_minute;
} S;

static volatile sig_atomic_t got_chld, got_term;

static void on_chld(int sig) { (void)sig; got_chld = 1; }
static void on_term(int sig) { (void)sig; got_term = 1; }

static int area_rows(void)
{
    int r = S.rows > 0 ? S.rows - 1 : S.def_rows;

    return r > 0 ? r : 1;
}

static int area_cols(void)
{
    return S.cols > 0 ? S.cols : S.def_cols;
}

static void message(const char *text)
{
    snprintf(S.msg, sizeof S.msg, "%s", text);
    S.msg_until = now_ms() + 2000;
}

/* ---- windows ---------------------------------------------------------- */

static void write_all(int fd, const char *s, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, s, n);

        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return;
        s += w;
        n -= (size_t)w;
    }
}

/* The emulator answers queries (cursor position, ...) through this. */
static void vt_reply(void *ctx, const char *s, size_t n)
{
    write_all(((struct window *)ctx)->fd, s, n);
}

static const char *shell_path(void)
{
    const char *sh = getenv("SHELL");

    return sh != NULL && *sh != '\0' ? sh : "/bin/sh";
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');

    return s != NULL ? s + 1 : p;
}

/* Start a login shell on a new pseudo-terminal. */
static int spawn(struct window *w, int rows, int cols)
{
    struct winsize ws;
    char name[128], argv0[64];
    int m;
    pid_t pid;

    m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0)
        return -1;
    if (grantpt(m) != 0 || unlockpt(m) != 0 || ptsname(m) == NULL) {
        close(m);
        return -1;
    }
    snprintf(name, sizeof name, "%s", ptsname(m));
    memset(&ws, 0, sizeof ws);
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    snprintf(argv0, sizeof argv0, "-%s", base_name(shell_path()));

    pid = fork();
    if (pid < 0) {
        close(m);
        return -1;
    }
    if (pid == 0) {
        int s, fd;

        setsid();
        s = open(name, O_RDWR);
        if (s < 0)
            _exit(127);
#ifdef TIOCSCTTY
        ioctl(s, TIOCSCTTY, 0);
#endif
        ioctl(s, TIOCSWINSZ, &ws);
        dup2(s, 0);
        dup2(s, 1);
        dup2(s, 2);
        for (fd = 3; fd < 256; fd++)
            close(fd);
        signal(SIGCHLD, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        setenv("TERM", "screen-256color", 1);
        setenv("TM", S.socket_path, 1);
        unsetenv("COLUMNS");
        unsetenv("LINES");
        execl(shell_path(), argv0, (char *)NULL);
        _exit(127);
    }
    ioctl(m, TIOCSWINSZ, &ws);
    fcntl(m, F_SETFD, FD_CLOEXEC);
    w->fd = m;
    w->pid = pid;
    return 0;
}

static void select_window(struct window *w)
{
    if (w == NULL || w == S.cur)
        return;
    S.last = S.cur;
    S.cur = w;
    w->bell = 0;
    S.help = 0;
    if (S.prompt == P_SEARCH_DOWN || S.prompt == P_SEARCH_UP)
        S.prompt = P_NONE;
}

static struct window *new_window(void)
{
    struct window *w, **pp;
    int id = 0;

    /* the lowest free number */
    for (;;) {
        for (w = S.windows; w != NULL && w->id != id; w = w->next)
            ;
        if (w == NULL)
            break;
        id++;
    }
    w = xcalloc(1, sizeof *w);
    w->id = id;
    vt_init(&w->vt, area_rows(), area_cols(), S.cfg->history_dir,
            S.cfg->history_disk, S.cfg->history_ram);
    w->vt.reply = vt_reply;
    w->vt.reply_ctx = w;
    if (spawn(w, area_rows(), area_cols()) != 0) {
        vt_free(&w->vt);
        free(w);
        message("cannot start a shell");
        return NULL;
    }
    snprintf(w->name, sizeof w->name, "%s", base_name(shell_path()));
    for (pp = &S.windows; *pp != NULL && (*pp)->id < id; pp = &(*pp)->next)
        ;
    w->next = *pp;
    *pp = w;
    select_window(w);
    return w;
}

static void shutdown_server(void)
{
    struct window *w;

    if (S.cfd >= 0)
        msg_send(S.cfd, MSG_EXIT, "", 0);
    for (w = S.windows; w != NULL; w = w->next)
        kill(-w->pid, SIGHUP);
    unlink(S.socket_path);
    exit(0);
}

static void remove_window(struct window *w)
{
    struct window **pp;

    for (pp = &S.windows; *pp != NULL && *pp != w; pp = &(*pp)->next)
        ;
    if (*pp == NULL)
        return;
    *pp = w->next;
    close(w->fd);
    kill(-w->pid, SIGHUP);
    if (S.last == w)
        S.last = NULL;
    if (S.cur == w) {
        S.cur = S.last != NULL ? S.last : S.windows;
        S.last = NULL;
        if (S.cur != NULL)
            S.cur->bell = 0;
    }
    if (S.shown == w)
        S.shown = NULL;
    vt_free(&w->vt);
    free(w);
    if (S.windows == NULL)
        shutdown_server();
    S.prompt = P_NONE;
    S.help = 0;
}

static void reap(void)
{
    struct window *w;
    pid_t pid;
    int st;

    got_chld = 0;
    while ((pid = waitpid(-1, &st, WNOHANG)) > 0)
        for (w = S.windows; w != NULL; w = w->next)
            if (w->pid == pid)
                w->dead = 1;
}

static void resize_windows(void)
{
    struct winsize ws;
    struct window *w;

    memset(&ws, 0, sizeof ws);
    ws.ws_row = (unsigned short)area_rows();
    ws.ws_col = (unsigned short)area_cols();
    for (w = S.windows; w != NULL; w = w->next) {
        vt_resize(&w->vt, area_rows(), area_cols());
        ioctl(w->fd, TIOCSWINSZ, &ws);
    }
}

/* Name windows after what runs in them (the terminal's foreground
 * process), unless the user named them. Linux only: uses /proc. */
static int update_names(void)
{
    struct window *w;
    int changed = 0;

    for (w = S.windows; w != NULL; w = w->next) {
        char path[64], name[32];
        pid_t pg;
        FILE *f;

        if (w->renamed || (pg = tcgetpgrp(w->fd)) <= 0)
            continue;
        snprintf(path, sizeof path, "/proc/%ld/comm", (long)pg);
        f = fopen(path, "r");
        if (f == NULL)
            continue;
        if (fgets(name, sizeof name, f) != NULL) {
            name[strcspn(name, "\n")] = '\0';
            if (name[0] != '\0' && strcmp(name, w->name) != 0) {
                snprintf(w->name, sizeof w->name, "%s", name);
                changed = 1;
            }
        }
        fclose(f);
    }
    return changed;
}

/* ---- the client -------------------------------------------------------- */

static void drop_client(int detach)
{
    if (S.cfd < 0)
        return;
    if (detach)
        msg_send(S.cfd, MSG_DETACH, "", 0);
    close(S.cfd);
    S.cfd = -1;
    S.rows = S.cols = 0;
    S.inlen = 0;
    term_free(&S.term);
}

static void set_size(int rows, int cols);
static void client_frames(void);

/* A new connection says what it wants in its first frame: MSG_RESIZE to
 * attach (taking over from any attached client), MSG_EXIT to kill the
 * session. Anything else, or nothing within a second (a liveness probe
 * from "tm ls"), just closes it. */
static void accept_client(void)
{
    unsigned char b[64];
    const unsigned char *data;
    struct pollfd p;
    size_t len = 0, n, size = 0;
    int fd = accept(S.lfd, NULL, NULL), type = 0;

    if (fd < 0)
        return;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    p.fd = fd;
    p.events = POLLIN;
    while (size == 0 && len < sizeof b && poll(&p, 1, 1000) > 0) {
        ssize_t r = read(fd, b + len, sizeof b - len);

        if (r <= 0)
            break;
        len += (size_t)r;
        size = msg_parse(b, len, &type, &data, &n);
    }
    if (size > 0 && type == MSG_EXIT) {
        close(fd);
        shutdown_server();
    }
    if (size == 0 || type != MSG_RESIZE || n != 4) {
        close(fd);
        return;
    }
    drop_client(1);                 /* one client at a time */
    S.cfd = fd;
    S.fresh = 1;
    memcpy(S.in, b + size, len - size);
    S.inlen = len - size;
    set_size(data[0] << 8 | data[1], data[2] << 8 | data[3]);
    client_frames();
}

static void set_size(int rows, int cols)
{
    if (rows < 2)
        rows = 2;
    if (cols < 10)
        cols = 10;
    S.rows = rows;
    S.cols = cols;
    S.def_rows = rows - 1;
    S.def_cols = cols;
    resize_windows();
    term_reset(&S.term, rows, cols);
    S.term.app_cursor = S.term.app_keypad = -1;     /* unknown: resend */
    S.term.mouse = -1;
    S.term.paste = -1;
    S.scr = xrealloc(S.scr, sizeof *S.scr * (size_t)rows * (size_t)cols);
    S.mask = xrealloc(S.mask, (size_t)rows);
    S.full = 1;
    if (S.fresh) {
        /* alternate screen, no auto-wrap (the bottom-right cell must not
         * scroll), clear */
        buf_str(&S.out, "\x1b[?1049h\x1b[?7l\x1b[0m\x1b[H\x1b[2J");
        S.fresh = 0;
    }
}

static void paste(void)
{
    char *s;
    size_t i;

    if (S.paste == NULL || S.cur == NULL)
        return;
    s = xmalloc(S.paste_len);
    for (i = 0; i < S.paste_len; i++)   /* lines end with CR, as typed */
        s[i] = S.paste[i] == '\n' ? '\r' : S.paste[i];
    if (S.cur->vt.bracketed_paste)
        write_all(S.cur->fd, "\x1b[200~", 6);
    write_all(S.cur->fd, s, S.paste_len);
    if (S.cur->vt.bracketed_paste)
        write_all(S.cur->fd, "\x1b[201~", 6);
    free(s);
}

static void prefix_command(int k)
{
    struct window *w;
    int rows = area_rows();

    switch (k) {
    case 'c':
        new_window();
        break;
    case 'n':
        select_window(S.cur->next != NULL ? S.cur->next : S.windows);
        break;
    case 'p':
        /* the one before the current, or the last if it is the first */
        for (w = S.windows; w->next != NULL && w->next != S.cur; w = w->next)
            ;
        select_window(w);
        break;
    case 'l':
        if (S.last != NULL)
            select_window(S.last);
        break;
    case 'd':
        drop_client(1);
        break;
    case ',':
        S.prompt = P_RENAME;
        snprintf(S.pbuf, sizeof S.pbuf, "%s", S.cur->name);
        break;
    case '&':
    case 'x':
        S.prompt = P_KILL;
        S.pbuf[0] = '\0';
        break;
    case '[':
        copy_enter(S.cur, rows);
        break;
    case K_PGUP:
        copy_enter(S.cur, rows);
        copy_key(S.cur, K_PGUP, rows, NULL, NULL);
        break;
    case ']':
        paste();
        break;
    case '?':
        S.help = 1;
        break;
    case 'm':
        S.mouse = !S.mouse;
        message(S.mouse ? "Mouse on" : "Mouse off");
        break;
    case 'r':
        if (S.rows > 0)
            term_reset(&S.term, S.rows, S.cols);
        S.full = 1;
        break;
    case PREFIX: {
        char c = PREFIX;

        write_all(S.cur->fd, &c, 1);
        break;
    }
    default:
        if (k >= '0' && k <= '9') {
            for (w = S.windows; w != NULL && w->id != k - '0'; w = w->next)
                ;
            if (w != NULL)
                select_window(w);
            else
                message("no such window");
        }
        break;
    }
}

static void prompt_key(int k)
{
    size_t len = strlen(S.pbuf);
    int kind = S.prompt;

    if (kind == P_KILL) {           /* y confirms, anything else cancels */
        S.prompt = P_NONE;
        if (k == 'y' || k == 'Y')
            remove_window(S.cur);
        return;
    }
    if (k == K_ENTER) {
        S.prompt = P_NONE;
        if (kind == P_RENAME) {
            snprintf(S.cur->name, sizeof S.cur->name, "%s", S.pbuf);
            S.cur->renamed = S.pbuf[0] != '\0';
        } else if (!copy_search(S.cur, S.pbuf, kind == P_SEARCH_UP,
                                area_rows())) {
            message("Pattern not found");
        }
    } else if (k == K_ESC || k == K_CTRL('c') || k == K_CTRL('g')) {
        S.prompt = P_NONE;
    } else if (k == K_BS) {
        while (len > 0 && (S.pbuf[--len] & 0xC0) == 0x80)
            ;
        S.pbuf[len] = '\0';
    } else if (k == K_CTRL('u')) {
        S.pbuf[0] = '\0';
    } else if (k >= 32 && k < 256 && len + 1 < sizeof S.pbuf) {
        S.pbuf[len] = (char)k;
        S.pbuf[len + 1] = '\0';
    }
}

/* Keep copied text for C-b ], and hand it to the outer terminal's
 * clipboard too (OSC 52; terminals that don't support it ignore it). */
static void take_yank(char *yank, size_t len)
{
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char m[48];
    size_t i;

    if (yank == NULL)
        return;
    free(S.paste);
    S.paste = yank;
    S.paste_len = len;
    snprintf(m, sizeof m, "Copied %lu bytes", (unsigned long)len);
    message(m);

    buf_str(&S.out, "\x1b]52;c;");
    for (i = 0; i < len; i += 3) {
        unsigned v = (unsigned char)yank[i] << 16;
        char q[4];

        if (i + 1 < len)
            v |= (unsigned char)yank[i + 1] << 8;
        if (i + 2 < len)
            v |= (unsigned char)yank[i + 2];
        q[0] = b64[v >> 18 & 63];
        q[1] = b64[v >> 12 & 63];
        q[2] = i + 1 < len ? b64[v >> 6 & 63] : '=';
        q[3] = i + 2 < len ? b64[v & 63] : '=';
        buf_add(&S.out, q, 4);
    }
    buf_str(&S.out, "\a");
}

static void copy_mode_key(int k)
{
    char *yank = NULL;
    size_t len = 0;
    int r = copy_key(S.cur, k, area_rows(), &yank, &len);

    if (r == COPY_SEARCH_DOWN || r == COPY_SEARCH_UP) {
        S.prompt = r == COPY_SEARCH_DOWN ? P_SEARCH_DOWN : P_SEARCH_UP;
        S.pbuf[0] = '\0';
    }
    take_yank(yank, len);
}

/* ---- the mouse ---------------------------------------------------------- */

/* Pass a mouse event to a program that asked for them, in the form and
 * amount it asked for. */
static void mouse_forward(struct window *w, const struct mouse *m)
{
    int mode = w->vt.mouse, b = m->b;
    char seq[32];
    int n;

    if ((b & MB_MOTION) && (b & 3) == 3 && !(mode & MOUSE_ANY))
        return;                     /* plain movement */
    if ((b & MB_MOTION) && !(mode & (MOUSE_DRAG | MOUSE_ANY)))
        return;                     /* dragging */
    if (m->y >= w->vt.rows || m->x >= w->vt.cols)
        return;
    if (mode & MOUSE_SGR) {
        n = snprintf(seq, sizeof seq, "\x1b[<%d;%d;%d%c", b, m->x + 1,
                     m->y + 1, m->release ? 'm' : 'M');
    } else {                        /* the old X10 encoding */
        if (m->release && (b & MB_WHEEL))
            return;
        if (m->release)
            b = (b & ~3) | 3;
        if (m->x > 222 || m->y > 222)
            return;
        n = snprintf(seq, sizeof seq, "\x1b[M%c%c%c", 32 + b, 33 + m->x,
                     33 + m->y);
    }
    write_all(w->fd, seq, (size_t)n);
}

static void mouse_status(const struct mouse *m)
{
    struct window *w;
    int i;

    if (m->release || (m->b & MB_MOTION))
        return;
    if (m->b & MB_WHEEL) {
        prefix_command((m->b & 1) ? 'n' : 'p');
        return;
    }
    if ((m->b & 3) != 0)
        return;
    for (i = 0; i < S.ntabs; i++) {
        if (m->x < S.tab_x0[i] || m->x >= S.tab_x1[i])
            continue;
        for (w = S.windows; w != NULL && w->id != S.tab_id[i]; w = w->next)
            ;
        select_window(w);
    }
}

static void mouse_event(const struct mouse *m)
{
    struct window *w = S.cur;
    int rows = area_rows(), wheel = m->b & MB_WHEEL;
    int left = (m->b & (3 | MB_WHEEL)) == 0;
    int drag = (m->b & MB_MOTION) && left;

    if (m->b < 0 || w == NULL)
        return;
    if (S.help) {
        if (!m->release && !(m->b & MB_MOTION))
            S.help = 0;
        return;
    }
    if (S.prompt != P_NONE)
        return;
    if (m->y >= rows && !S.dragging) {
        mouse_status(m);
        return;
    }

    /* the program wants the mouse itself (Shift overrides, as in xterm) */
    if (!w->cm.active && (w->vt.mouse & (MOUSE_CLICK | MOUSE_DRAG |
                                         MOUSE_ANY)) && !(m->b & MB_SHIFT)) {
        mouse_forward(w, m);
        return;
    }

    if (wheel) {
        if (m->release)
            return;
        if (w->cm.active) {
            if (copy_scroll(w, (m->b & 1) ? 3 : -3, rows) &&
                (m->b & 1) && !w->cm.selecting)
                w->cm.active = 0;   /* back at the bottom: done */
        } else if (w->vt.g == &w->vt.alt) {
            /* a full-screen program without mouse support (less, man):
             * the wheel moves it with the arrow keys */
            const char *k = (m->b & 1)
                ? (w->vt.app_cursor ? "\x1bOB" : "\x1b[B")
                : (w->vt.app_cursor ? "\x1bOA" : "\x1b[A");
            int i;

            for (i = 0; i < 3; i++)
                write_all(w->fd, k, 3);
        } else if (!(m->b & 1)) {
            copy_enter(w, rows);
            copy_scroll(w, -3, rows);
        }
        return;
    }

    if (left && !m->release && !(m->b & MB_MOTION)) {      /* press */
        S.pressed = 1;
        S.press_x = m->x;
        S.press_y = m->y;
        S.dragging = 0;
        if (w->cm.active)
            copy_point(w, m->x, m->y, rows, 0);
    } else if (drag && S.pressed) {
        if (!S.dragging) {
            S.dragging = 1;
            if (!w->cm.active)
                copy_enter(w, rows);
            copy_point(w, S.press_x, S.press_y, rows, 0);
        }
        copy_point(w, m->x, m->y, rows, 1);
    } else if (m->release && (m->b & 3) == 0) {
        if (S.dragging && w->cm.active) {
            char *yank = NULL;
            size_t len = 0;

            copy_finish(w, &yank, &len);
            take_yank(yank, len);
        }
        S.pressed = S.dragging = 0;
    }
}

/* Keystrokes (and, with the mouse on, mouse reports) from the client. */
static void handle_input(const unsigned char *in, size_t n)
{
    static unsigned char join[sizeof S.kpend + MSG_MAX];
    const unsigned char *s = in;
    size_t i = 0, used;
    struct mouse m;
    int r;

    if (S.kpend_len > 0) {          /* finish a report cut off last time */
        memcpy(join, S.kpend, S.kpend_len);
        memcpy(join + S.kpend_len, in, n);
        s = join;
        n += S.kpend_len;
        S.kpend_len = 0;
    }
    while (i < n && S.cur != NULL) {
        int k;

        if (S.mouse && s[i] == 27 &&
            (r = mouse_parse(s + i, n - i, &m, &used)) != 0) {
            if (r < 0) {
                S.kpend_len = n - i < sizeof S.kpend ? n - i : 0;
                memcpy(S.kpend, s + i, S.kpend_len);
                return;
            }
            i += used;
            mouse_event(&m);
            S.redraw = 1;
            continue;
        }
        if (S.prefix) {
            S.prefix = 0;
            k = key_next(s + i, n - i, &used);
            i += used;
            S.help = 0;
            prefix_command(k);
            S.redraw = 1;
            continue;
        }
        if (S.help || S.prompt != P_NONE || S.cur->cm.active) {
            k = key_next(s + i, n - i, &used);
            i += used;
            if (S.prompt != P_NONE)
                prompt_key(k);
            else if (k == PREFIX)
                S.prefix = 1;
            else if (S.help)
                S.help = 0;
            else
                copy_mode_key(k);
            S.redraw = 1;
            continue;
        }
        /* pass everything up to the next prefix key (or mouse report) to
         * the program */
        {
            size_t j = i;

            while (j < n && s[j] != PREFIX &&
                   !(S.mouse && s[j] == 27 && j > i))
                j++;
            write_all(S.cur->fd, (const char *)s + i, j - i);
            i = j;
            if (i < n && s[i] == PREFIX) {
                S.prefix = 1;
                i++;
            }
        }
    }
}

/* Act on the complete frames in S.in. */
static void client_frames(void)
{
    size_t off = 0, size, n;
    const unsigned char *data;
    int type;

    while ((size = msg_parse(S.in + off, S.inlen - off, &type, &data, &n))) {
        off += size;
        if (type == MSG_INPUT)
            handle_input(data, n);
        else if (type == MSG_RESIZE && n == 4)
            set_size(data[0] << 8 | data[1], data[2] << 8 | data[3]);
        else if (type == MSG_EXIT)
            shutdown_server();
        if (S.cfd < 0)
            return;                 /* detached by that input */
    }
    memmove(S.in, S.in + off, S.inlen - off);
    S.inlen -= off;
    if (S.inlen == sizeof S.in)     /* garbage: no frame fits */
        S.inlen = 0;
}

static void client_read(void)
{
    ssize_t r = read(S.cfd, S.in + S.inlen, sizeof S.in - S.inlen);

    if (r <= 0) {
        if (r < 0 && (errno == EINTR || errno == EAGAIN))
            return;
        drop_client(0);
        return;
    }
    S.inlen += (size_t)r;
    client_frames();
}

/* ---- composing the screen ---------------------------------------------- */

/* Write UTF-8 text into a row of cells from column x; returns the column
 * after it. */
static int put_text(struct cell *row, int cols, int x, const char *s,
                    uint32_t attr)
{
    size_t n = strlen(s);

    while (n > 0 && x < cols) {
        int used;
        uint32_t cp = utf8_decode((const unsigned char *)s, n, &used);

        if (used == 0)
            break;
        s += used;
        n -= (size_t)used;
        row[x].ch = cp;
        row[x].attr = attr;
        x++;
    }
    return x;
}

static void fill(struct cell *row, int cols, uint32_t attr)
{
    int x;

    for (x = 0; x < cols; x++) {
        row[x].ch = ' ';
        row[x].attr = attr;
    }
}

/* The bottom line: prompt, message, or tmux's window list and clock. */
static int draw_status(struct cell *row, int cols)
{
    char tmp[160], clock[64];
    struct window *w;
    time_t t = time(NULL);
    int x;

    if (S.prompt != P_NONE) {
        static const char *const label[] = {
            "", "(rename-window) ", "", "(search down) ", "(search up) "
        };

        fill(row, cols, A_MESSAGE);
        if (S.prompt == P_KILL) {
            snprintf(tmp, sizeof tmp, "kill-window %d:%s? (y/n)",
                     S.cur->id, S.cur->name);
            return put_text(row, cols, 0, tmp, A_MESSAGE);
        }
        x = put_text(row, cols, 0, label[S.prompt], A_MESSAGE);
        return put_text(row, cols, x, S.pbuf, A_MESSAGE);
    }
    if (S.msg[0] != '\0' && now_ms() < S.msg_until) {
        fill(row, cols, A_MESSAGE);
        put_text(row, cols, 0, S.msg, A_MESSAGE);
        return -1;
    }

    fill(row, cols, A_STATUS);
    S.ntabs = 0;
    snprintf(tmp, sizeof tmp, "[%s] ", S.cfg->session);
    x = put_text(row, cols, 0, tmp, A_STATUS);
    for (w = S.windows; w != NULL; w = w->next) {
        snprintf(tmp, sizeof tmp, "%d:%s%s ", w->id, w->name,
                 w == S.cur ? "*" : w == S.last ? "-" : w->bell ? "!" : "");
        if (S.ntabs < 16) {         /* for clicks */
            S.tab_x0[S.ntabs] = x;
            S.tab_id[S.ntabs] = w->id;
        }
        x = put_text(row, cols, x, tmp, A_STATUS);
        if (S.ntabs < 16)
            S.tab_x1[S.ntabs++] = x;
    }
    strftime(clock, sizeof clock, "%H:%M %d-%b-%y", localtime(&t));
    snprintf(tmp, sizeof tmp, "\"%s\" %s", S.host, clock);
    if ((int)strlen(tmp) + x + 1 < cols)
        put_text(row, cols, cols - (int)strlen(tmp) - 1, tmp, A_STATUS);
    return -1;
}

/* Copy row y of a window's screen into the frame, padding or cutting it
 * to the frame's width. */
static void copy_row(struct cell *dst, const struct vt *vt, int y, int cols)
{
    int n = vt->cols < cols ? vt->cols : cols, x;

    if (y < vt->rows) {
        memcpy(dst, &vt->g->c[y * vt->cols], sizeof *dst * (size_t)n);
    } else {
        n = 0;
    }
    for (x = n; x < cols; x++) {
        dst[x].ch = ' ';
        dst[x].attr = ATTR_DEFAULT;
    }
}

/* Draw a frame and send it. Normally only the rows the program changed
 * are copied and compared (one row, for a typed character); everything
 * is redone when the view changes. */
static void render(void)
{
    struct window *w = S.cur, *o;
    int rows = area_rows(), cols = area_cols(), y, cx = -1, cy = -1, px;
    int mouse, mode, full;

    if (S.cfd < 0 || S.rows == 0 || w == NULL)
        return;
    mode = S.help ? VIEW_HELP : w->cm.active ? VIEW_COPY : VIEW_WINDOW;
    full = S.full || w != S.shown || mode != S.shown_mode ||
        mode != VIEW_WINDOW || w->vt.rows != rows || w->vt.cols != cols;
    memset(S.mask, full, (size_t)S.rows);

    if (S.help) {
        for (y = 0; y < rows; y++) {
            fill(S.scr + y * cols, cols, ATTR_DEFAULT);
            if (y < (int)(sizeof help_text / sizeof *help_text))
                put_text(S.scr + y * cols, cols, 0, help_text[y],
                         ATTR_DEFAULT);
        }
    } else if (w->cm.active) {
        copy_draw(w, S.scr, rows, cols, &cx, &cy);
    } else {
        for (y = 0; y < rows; y++) {
            if (!full && (y >= w->vt.rows || !w->vt.rowdirty[y]))
                continue;
            copy_row(S.scr + y * cols, &w->vt, y, cols);
            S.mask[y] = 1;
        }
        if (w->vt.cursor_visible) {
            cx = w->vt.x;
            cy = w->vt.y;
        }
    }
    px = draw_status(S.scr + rows * cols, cols);
    S.mask[rows] = 1;
    if (px >= 0) {                  /* typing in a prompt */
        cx = px < cols ? px : cols - 1;
        cy = rows;
    }

    if (S.mouse)                    /* we want clicks and drags, as SGR */
        mouse = MOUSE_DRAG | MOUSE_SGR |
            (w->cm.active ? 0 : (w->vt.mouse & MOUSE_ANY));
    else                            /* the program's own modes, as is */
        mouse = w->cm.active ? 0 : w->vt.mouse;
    render_modes(&S.out, &S.term, w->vt.app_cursor, w->vt.app_keypad,
                 mouse, w->vt.bracketed_paste);
    render_frame(&S.out, &S.term, S.scr, full ? NULL : S.mask, cx, cy);
    if (w->vt.bell)
        buf_add(&S.out, "\a", 1);
    for (o = S.windows; o != NULL; o = o->next) {
        if (o->vt.bell && o != w)
            o->bell = 1;
        o->vt.bell = 0;
        o->vt.dirty = 0;
        memset(o->vt.rowdirty, 0, (size_t)o->vt.rows);
    }
    S.full = 0;
    S.redraw = 0;
    S.shown = w;
    S.shown_mode = mode;
    if (S.out.len > 0 &&
        msg_send_all(S.cfd, MSG_OUTPUT, S.out.s, S.out.len) != 0)
        drop_client(0);
    S.out.len = 0;
    S.last_frame = now_ms();
}

/* ---- main loop --------------------------------------------------------- */

void server_run(int listen_fd, const char *socket_path,
                const struct server_config *cfg, int rows, int cols)
{
    struct sigaction sa;
    static unsigned char buf[4096];
    int pending = 0;

    memset(&S, 0, sizeof S);
    S.cfg = cfg;
    S.socket_path = socket_path;
    S.lfd = listen_fd;
    S.cfd = -1;
    S.def_rows = rows - 1 > 0 ? rows - 1 : 1;
    S.def_cols = cols;
    S.mouse = getenv("TM_MOUSE") == NULL || strcmp(getenv("TM_MOUSE"), "0");
    gethostname(S.host, sizeof S.host - 1);
    S.host[strcspn(S.host, ".")] = '\0';

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_chld;
    sigaction(SIGCHLD, &sa, NULL);
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, SIG_IGN);

    if (new_window() == NULL)
        exit(1);

    for (;;) {
        struct pollfd pfd[64];
        struct window *w, *next;
        int n = 0, timeout = 1000, dirty = 0, i, cfd = S.cfd;
        long long now;

        pfd[n].fd = S.lfd;
        pfd[n++].events = POLLIN;
        if (cfd >= 0) {
            pfd[n].fd = S.cfd;
            pfd[n++].events = POLLIN;
        }
        for (w = S.windows; w != NULL && n < 64; w = w->next) {
            pfd[n].fd = w->fd;
            pfd[n++].events = POLLIN;
        }
        if (pending) {                  /* until the next frame is due */
            long long due = S.last_frame + FRAME_MS - now_ms();

            timeout = due < 1 ? 0 : (int)due;
        }
        if (poll(pfd, (nfds_t)n, timeout) < 0 && errno != EINTR)
            continue;

        if (got_term)
            shutdown_server();
        if (got_chld)
            reap();

        /* the client first (keys before output), then a new client */
        if (cfd >= 0 && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR)))
            client_read();
        if (pfd[0].revents & POLLIN)
            accept_client();
        for (i = cfd >= 0 ? 2 : 1; i < n; i++) {
            if (!pfd[i].revents)
                continue;
            for (w = S.windows; w != NULL && w->fd != pfd[i].fd; w = w->next)
                ;
            if (w == NULL)
                continue;
            {
                ssize_t r = read(w->fd, buf, sizeof buf);

                if (r > 0)
                    vt_write(&w->vt, buf, (size_t)r);
                else if (r == 0 || (errno != EINTR && errno != EAGAIN))
                    w->dead = 1;
            }
        }
        for (w = S.windows; w != NULL; w = next) {
            next = w->next;
            if (w->dead)
                remove_window(w);
        }

        now = now_ms();
        if (now - S.last_names >= 1000) {
            time_t t = time(NULL);
            int minute = (int)(t / 60);

            if (update_names())
                dirty = 1;
            S.last_names = now;
            if (minute != S.last_minute) {      /* the clock */
                S.last_minute = minute;
                dirty = 1;
            }
            if (S.msg[0] != '\0' && now >= S.msg_until) {
                S.msg[0] = '\0';
                dirty = 1;
            }
        }
        for (w = S.windows; w != NULL; w = w->next)
            if (w->vt.dirty || w->vt.bell)
                dirty = 1;
        if (S.out.len > 0 || S.redraw)
            dirty = 1;
        /* Draw now if the last frame is old enough, else when it is.
         * Keystrokes passed to a program draw nothing themselves: the
         * echo that comes back does. */
        if (dirty || pending) {
            if (now - S.last_frame >= FRAME_MS) {
                render();
                pending = 0;
            } else {
                pending = 1;
            }
        }
    }
}
