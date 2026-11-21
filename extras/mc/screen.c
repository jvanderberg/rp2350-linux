/*
 * screen.c - off-screen cell buffer, diff-based output, terminal modes.
 *
 * Two arrays of cells are kept: `back`, which the program draws into, and
 * `front`, which mirrors what the terminal currently shows. scr_flush()
 * walks both and emits cursor moves, colours and characters only for the
 * cells that differ, all collected in one buffer and written at once.
 *
 * Colours go out as xterm-256 indices (see SKIN_PALETTE in skin.h) so the
 * classic dark-blue look does not depend on the terminal's colour theme.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "screen.h"
#include "skin.h"
#include "util.h"

struct cell {
    unsigned ch;            /* Unicode code point */
    unsigned short attr;    /* ATTR(fg, bg) */
};

int scr_rows = 24, scr_cols = 80;
volatile sig_atomic_t scr_winch, scr_quit;

static struct cell *back, *front;
static int want_y = -1, want_x;     /* requested cursor position */
static int shown_y = -2, shown_x;   /* cursor as last sent (-2 unknown) */
static int need_clear;
static int is_open, is_raw;
static struct termios saved_tio;
static int use_256 = 1;
static const unsigned char palette[16] = SKIN_PALETTE;

/* ---- output buffer ---------------------------------------------------- */

static char *obuf;
static size_t olen, ocap;

static void out(const char *s, size_t n)
{
    if (olen + n > ocap) {
        size_t cap = ocap ? ocap * 2 : 8192;

        while (cap < olen + n)
            cap *= 2;
        obuf = realloc(obuf, cap);
        if (obuf == NULL) {
            olen = ocap = 0;
            return;
        }
        ocap = cap;
    }
    memcpy(obuf + olen, s, n);
    olen += n;
}

static void outs(const char *s)
{
    out(s, strlen(s));
}

static void out_flush(void)
{
    size_t off = 0;

    while (off < olen) {
        ssize_t n = write(STDOUT_FILENO, obuf + off, olen - off);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    olen = 0;
}

static void out_goto(int y, int x)
{
    char tmp[24];

    out(tmp, (size_t)sprintf(tmp, "\x1b[%d;%dH", y + 1, x + 1));
}

/* Append the SGR parameters for one colour. `base` is 30 (fg) or 40 (bg). */
static void out_color(int c, int base)
{
    char tmp[16];

    if (c == C_DEFAULT)
        sprintf(tmp, ";%d", base + 9);
    else if (use_256)
        sprintf(tmp, ";%d;5;%d", base + 8, palette[c]);
    else if (c < 8)
        sprintf(tmp, ";%d", base + c);
    else
        sprintf(tmp, ";%d", base + 60 + c - 8);    /* aixterm bright */
    outs(tmp);
}

static void out_attr(int a)
{
    outs("\x1b[0");
    out_color(ATTR_FG(a), 30);
    out_color(ATTR_BG(a), 40);
    outs("m");
}

static void out_char(unsigned c)
{
    char b[4];

    if (c < 0x20 || c == 0x7F)
        c = '?';
    if (c < 0x80) {
        b[0] = (char)c;
        out(b, 1);
    } else if (c < 0x800) {
        b[0] = (char)(0xC0 | (c >> 6));
        b[1] = (char)(0x80 | (c & 0x3F));
        out(b, 2);
    } else if (c < 0x10000) {
        b[0] = (char)(0xE0 | (c >> 12));
        b[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        b[2] = (char)(0x80 | (c & 0x3F));
        out(b, 3);
    } else {
        b[0] = (char)(0xF0 | (c >> 18));
        b[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((c >> 6) & 0x3F));
        b[3] = (char)(0x80 | (c & 0x3F));
        out(b, 4);
    }
}

/* ---- terminal modes --------------------------------------------------- */

static void on_winch(int sig)
{
    (void)sig;
    scr_winch = 1;
}

static void on_quit(int sig)
{
    (void)sig;
    scr_quit = 1;
}

static void set_raw(void)
{
    struct termios t = saved_tio;

    /* No echo, no line editing, no signals from ^C/^Z, no ^S/^Q flow
     * control (^S is quick search), no CR->NL translation. */
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG | IEXTEN);
    t.c_iflag &= ~(tcflag_t)(IXON | ICRNL | INLCR | ISTRIP | BRKINT);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
    is_raw = 1;
}

/* Alternate screen, no auto-wrap (so writing the bottom-right cell does
 * not scroll), SGR mouse reporting. */
static void enter_fullscreen(void)
{
    outs("\x1b[?1049h\x1b[?7l\x1b[?1000h\x1b[?1006h\x1b[?25l\x1b[0m\x1b[2J");
    out_flush();
    shown_y = -2;
}

static void leave_fullscreen(void)
{
    outs("\x1b[0m\x1b[?1000l\x1b[?1006l\x1b[?7h\x1b[?25h\x1b[?1049l");
    out_flush();
}

int scr_open(void)
{
    struct sigaction sa;
    const char *e = getenv("MC_16COLORS");

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return -1;
    if (tcgetattr(STDIN_FILENO, &saved_tio) != 0)
        return -1;
    use_256 = !(e != NULL && *e != '\0' && *e != '0');

    /* No SA_RESTART: a resize must interrupt the poll() in key_get(). */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, NULL);
    sa.sa_handler = on_quit;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    set_raw();
    enter_fullscreen();
    is_open = 1;
    scr_update_size();
    scr_invalidate();
    return 0;
}

void scr_close(void)
{
    if (!is_open)
        return;
    leave_fullscreen();
    if (is_raw)
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_tio);
    is_raw = 0;
    is_open = 0;
}

void scr_suspend(void)
{
    leave_fullscreen();
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_tio);
    is_raw = 0;
}

void scr_resume(void)
{
    set_raw();
    enter_fullscreen();
    scr_update_size();
    scr_invalidate();
}

int scr_update_size(void)
{
    struct winsize ws;
    int rows = 24, cols = 80;
    size_t n;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
        ws.ws_row > 0 && ws.ws_col > 0) {
        rows = ws.ws_row;
        cols = ws.ws_col;
    }
    if (back != NULL && rows == scr_rows && cols == scr_cols)
        return 0;
    scr_rows = rows;
    scr_cols = cols;
    n = (size_t)rows * (size_t)cols;
    free(back);
    free(front);
    back = xmalloc(n * sizeof *back);
    front = xmalloc(n * sizeof *front);
    scr_fill(0, 0, rows, cols, ' ', A_SHELL);
    scr_invalidate();
    return 1;
}

void scr_invalidate(void)
{
    int i, n = scr_rows * scr_cols;

    for (i = 0; i < n; i++) {
        front[i].ch = 0;
        front[i].attr = 0xFFFF;     /* matches no real attribute */
    }
    need_clear = 1;
    shown_y = -2;
}

/* ---- drawing ---------------------------------------------------------- */

void scr_put(int y, int x, unsigned ch, int attr)
{
    struct cell *c;

    if (y < 0 || y >= scr_rows || x < 0 || x >= scr_cols)
        return;
    c = &back[y * scr_cols + x];
    c->ch = ch;
    c->attr = (unsigned short)attr;
}

int scr_puts(int y, int x, const char *s, int attr)
{
    while (*s)
        scr_put(y, x++, utf8_next(&s), attr);
    return x;
}

void scr_text(int y, int x, int w, const char *s, int attr, int how)
{
    unsigned cp[512], fit[512];
    int n = 0, len, i, pad;

    if (w <= 0)
        return;
    if (w > 512)
        w = 512;
    while (*s && n < 512)
        cp[n++] = utf8_next(&s);

    if (n <= w) {
        memcpy(fit, cp, (size_t)n * sizeof *cp);
        len = n;
    } else if (how & T_TRIM_MID) {
        /* keep the start and the end, with a '~' where text was cut */
        int tail = (w - 1) / 2, head = w - 1 - tail;

        memcpy(fit, cp, (size_t)head * sizeof *cp);
        fit[head] = '~';
        memcpy(fit + head + 1, cp + n - tail, (size_t)tail * sizeof *cp);
        len = w;
    } else if ((how & T_TRIM_START) && w > 3) {
        fit[0] = fit[1] = fit[2] = '.';
        memcpy(fit + 3, cp + n - (w - 3), (size_t)(w - 3) * sizeof *cp);
        len = w;
    } else {
        memcpy(fit, cp, (size_t)w * sizeof *cp);
        len = w;
    }

    pad = w - len;
    if ((how & 3) == T_RIGHT)
        i = pad;
    else if ((how & 3) == T_CENTER)
        i = pad / 2;
    else
        i = 0;
    scr_fill(y, x, 1, w, ' ', attr);
    for (n = 0; n < len; n++)
        scr_put(y, x + i + n, fit[n], attr);
}

void scr_fill(int y, int x, int h, int w, unsigned ch, int attr)
{
    int r, c;

    for (r = y; r < y + h; r++)
        for (c = x; c < x + w; c++)
            scr_put(r, c, ch, attr);
}

void scr_hline(int y, int x, int w, unsigned ch, int attr)
{
    scr_fill(y, x, 1, w, ch, attr);
}

void scr_box(int y, int x, int h, int w, int attr)
{
    int r;

    scr_put(y, x, BOX_TL, attr);
    scr_hline(y, x + 1, w - 2, BOX_H, attr);
    scr_put(y, x + w - 1, BOX_TR, attr);
    for (r = y + 1; r < y + h - 1; r++) {
        scr_put(r, x, BOX_V, attr);
        scr_put(r, x + w - 1, BOX_V, attr);
    }
    scr_put(y + h - 1, x, BOX_BL, attr);
    scr_hline(y + h - 1, x + 1, w - 2, BOX_H, attr);
    scr_put(y + h - 1, x + w - 1, BOX_BR, attr);
}

/* Darken the cells a window at (y, x, h, w) would shade: two columns on
 * its right and one row below, offset like mc's shadows. The characters
 * underneath stay visible, only their colour changes. */
void scr_shadow(int y, int x, int h, int w)
{
    int r, c;

    for (r = y + 1; r <= y + h; r++)
        for (c = x + 2; c < x + w + 2; c++) {
            if (r < y + h && c < x + w)
                continue;           /* inside the window itself */
            if (r >= 0 && r < scr_rows && c >= 0 && c < scr_cols)
                back[r * scr_cols + c].attr = A_SHADOW;
        }
}

void scr_cursor(int y, int x)
{
    want_y = y;
    want_x = x;
}

void scr_flush(void)
{
    int y, x, attr = -1, ty = -1, tx = -1, drew = 0;

    if (need_clear) {
        outs("\x1b[0m\x1b[2J");
        need_clear = 0;
    }
    for (y = 0; y < scr_rows; y++) {
        for (x = 0; x < scr_cols; x++) {
            int i = y * scr_cols + x;

            if (back[i].ch == front[i].ch && back[i].attr == front[i].attr)
                continue;
            if (!drew) {
                outs("\x1b[?25l");  /* hide the cursor while painting */
                shown_y = -2;
                drew = 1;
            }
            if (ty != y || tx != x)
                out_goto(y, x);
            if (back[i].attr != attr) {
                attr = back[i].attr;
                out_attr(attr);
            }
            out_char(back[i].ch);
            front[i] = back[i];
            ty = y;
            tx = x + 1;
        }
    }
    if (want_y != shown_y || want_x != shown_x) {
        if (want_y >= 0) {
            out_goto(want_y, want_x);
            outs("\x1b[?25h");
        } else {
            outs("\x1b[?25l");
        }
        shown_y = want_y;
        shown_x = want_x;
    }
    out_flush();
}
