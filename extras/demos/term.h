/*
 * term.h - tiny ANSI terminal helper shared by the demos.
 *
 * Gives you: raw keyboard input, the alternate screen, a hidden cursor,
 * an output buffer that is written with one write() per frame, and a
 * clean restore on exit or Ctrl-C.
 */
#ifndef DEMO_TERM_H
#define DEMO_TERM_H

#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t term_quit_flag;
static struct termios term_saved;
static int term_raw;

static char *term_buf;
static size_t term_len, term_cap;

static void term_on_signal(int sig)
{
    (void)sig;
    term_quit_flag = 1;
}

static void term_flush(void)
{
    size_t off = 0;

    while (off < term_len) {
        ssize_t n = write(STDOUT_FILENO, term_buf + off, term_len - off);
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    term_len = 0;
}

static void term_put(const char *s, size_t n)
{
    if (term_len + n > term_cap) {
        size_t cap = term_cap ? term_cap : 4096;
        char *p;

        while (cap < term_len + n)
            cap *= 2;
        p = realloc(term_buf, cap);
        if (p == NULL) {
            term_flush();       /* out of memory: emit what we have */
            if (n > term_cap)
                return;
        } else {
            term_buf = p;
            term_cap = cap;
        }
    }
    memcpy(term_buf + term_len, s, n);
    term_len += n;
}

static void term_str(const char *s)
{
    term_put(s, strlen(s));
}

static void term_num(unsigned int v)
{
    char tmp[12];
    int i = (int)sizeof tmp;

    do {
        tmp[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    term_put(tmp + i, sizeof tmp - (size_t)i);
}

/* Move the cursor (1-based row and column). */
static void term_goto(int row, int col)
{
    term_str("\x1b[");
    term_num((unsigned int)row);
    term_put(";", 1);
    term_num((unsigned int)col);
    term_put("H", 1);
}

/* Set the xterm-256 background / foreground colour. */
static void term_bg(int color)
{
    term_str("\x1b[48;5;");
    term_num((unsigned int)color);
    term_put("m", 1);
}

static void term_fg(int color)
{
    term_str("\x1b[38;5;");
    term_num((unsigned int)color);
    term_put("m", 1);
}

static void term_reset(void)
{
    term_str("\x1b[0m");
}

static long long term_now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void term_size(int *cols, int *rows)
{
    struct winsize ws;
    const char *c = getenv("COLUMNS");
    const char *r = getenv("LINES");

    *cols = 80;
    *rows = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
        ws.ws_col > 0 && ws.ws_row > 0) {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    } else {
        if (c != NULL && atoi(c) > 0)
            *cols = atoi(c);
        if (r != NULL && atoi(r) > 0)
            *rows = atoi(r);
    }
}

/* Non-blocking: returns the next input byte, or -1 if there is none. */
static int term_read_byte(void)
{
    unsigned char c;

    if (read(STDIN_FILENO, &c, 1) == 1)
        return c;
    return -1;
}

static void term_begin(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = term_on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &term_saved) == 0) {
        struct termios raw = term_saved;

        raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        term_raw = 1;
    } else {
        /* Piped input: make reads non-blocking instead. */
        int fl = fcntl(STDIN_FILENO, F_GETFL);
        if (fl >= 0)
            fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);
    }

    /* Alternate screen, hide cursor, clear. */
    term_str("\x1b[?1049h\x1b[?25l\x1b[0m\x1b[2J");
    term_flush();
}

static void term_end(void)
{
    term_str("\x1b[0m\x1b[?25h\x1b[?1049l");
    term_flush();
    if (term_raw)
        tcsetattr(STDIN_FILENO, TCSANOW, &term_saved);
}

#endif /* DEMO_TERM_H */
