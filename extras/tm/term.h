/*
 * term.h - a terminal in part of an X11 window: a program on a
 * pseudo-terminal, shown through tm's terminal emulator (vt.c).
 *
 * Header-only, like x11.h (which must be included first, in the same
 * file): xterm uses one for its whole window, xmc for its output view.
 * Link with vt.c, sb.c, lz.c and util.c.
 *
 *   struct term t;
 *   term_init(&t, 13);                  fonts (13 px) and colours
 *   term_place(&t, x, y, w, h);         where it goes (again on a resize)
 *   term_start(&t, dir, argv);          run argv (NULL: a login shell)
 *   ... poll term_fd(&t) with the X connection; when it's readable:
 *   term_pump(&t);                      read the output; t.running drops
 *                                       to 0 when the program has gone
 *   term_key(&t, &e); term_mouse(&t, &e);
 *   term_draw(&t);                      send what changed, then x_present
 *   term_free(&t);                      hang up and free (term_start may
 *                                       use it again)
 *
 * Several can share the fonts: copy one after term_init (before it has
 * started) instead of calling term_init again.
 *
 * Programs see TERM=screen-256color, which is what vt.c implements. The
 * wheel scrolls back through history unless the program asked for the
 * mouse (or, on the alternate screen, as less and man use, it is arrow
 * keys); Shift goes to the history always.
 *
 * Only what changed is sent: the cells that differ from the last frame,
 * as runs of one colour; when the text scrolls, one copy on the server
 * moves the lines that are still there.
 */
#ifndef TERM_H
#define TERM_H

#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>

#include "vt.h"

int char_width(uint32_t cp);            /* util.c */

struct term {
    struct vt vt;
    int started;                        /* vt is set up */
    int ptm;                            /* our side of the pty, or -1 */
    pid_t pid;
    int running;                        /* the program still has the pty */
    int reaped, status;                 /* waitpid's answer, once it came */
    int focus;                          /* show the cursor */

    int x, y, w, h;                     /* the area, in pixels */
    int cw, ch, asc;                    /* cell size, baseline */
    int f_norm, f_bold, unicode;
    unsigned palette[256], fg, bg, cursor;

    int view;                           /* lines scrolled back into history */
    int held;                           /* mouse button down, for reports */

    /* what the window shows, to send only what changes */
    struct cell *shown, *tmp;
    unsigned *shown_hash, *hash;
    int shown_rows, shown_cols, shown_cx, shown_cy;
};

static inline void *term_alloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);

    if (p == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return p;
}

/* ---------- colours and fonts ---------- */

/* The colours a cell is drawn in. */
static inline void term_colors(const struct term *t, const struct cell *c, unsigned *fg, unsigned *bg)
{
    unsigned a = c->attr, f = CELL_FG(a), b = CELL_BG(a), fl = CELL_FL(a), s;

    if ((fl & AT_BOLD) && f < 8)
        f += 8;                         /* bold: the bright colour */
    *fg = f == COL_DEFAULT ? t->fg : t->palette[f];
    *bg = b == COL_DEFAULT ? t->bg : t->palette[b];
    if (fl & AT_REVERSE) {
        s = *fg;
        *fg = *bg;
        *bg = s;
    }
    if (fl & AT_HIDDEN)
        *fg = *bg;
}

/* Fonts (the first of misc-fixed, DejaVu Sans Mono and Lucida Typewriter
 * the server has, at px pixels) and colours (xpulse's dark theme; set
 * t->bg, fg and cursor after this to change them). */
static inline void term_init(struct term *t, int px)
{
    static const char *const fams[][2] = {
        { "-misc-fixed-medium-r-normal--%d-*-*-*-c-*-iso10646-1",
          "-misc-fixed-bold-r-normal--%d-*-*-*-c-*-iso10646-1" },
        { "-*-dejavu sans mono-medium-r-normal--%d-*-*-*-m-*-iso10646-1",
          "-*-dejavu sans mono-bold-r-normal--%d-*-*-*-m-*-iso10646-1" },
    };
    static const unsigned char base[16][3] = {
        { 30, 35, 42 },    { 248, 81, 73 },   { 63, 185, 80 },   { 210, 153, 34 },
        { 88, 166, 255 },  { 188, 140, 255 }, { 57, 197, 207 },  { 201, 209, 217 },
        { 110, 118, 129 }, { 255, 123, 114 }, { 86, 211, 100 },  { 227, 179, 65 },
        { 121, 192, 255 }, { 210, 168, 255 }, { 86, 212, 221 },  { 240, 246, 252 },
    };
    static const int cube[6] = { 0, 95, 135, 175, 215, 255 };
    char name[160];

    memset(t, 0, sizeof *t);
    t->ptm = -1;
    t->held = -1;
    t->shown_cx = t->shown_cy = -1;
    for (int i = 0; i < 16; i++)
        t->palette[i] = x_rgb(base[i][0], base[i][1], base[i][2]);
    for (int i = 0; i < 216; i++)
        t->palette[16 + i] = x_rgb(cube[i / 36], cube[i / 6 % 6], cube[i % 6]);
    for (int i = 0; i < 24; i++)
        t->palette[232 + i] = x_rgb(8 + 10 * i, 8 + 10 * i, 8 + 10 * i);
    t->bg = x_rgb(13, 17, 23);
    t->fg = x_rgb(230, 237, 243);
    t->cursor = x_rgb(88, 166, 255);

    for (size_t i = 0; i < sizeof fams / sizeof fams[0] && !t->f_norm; i++) {
        snprintf(name, sizeof name, fams[i][0], px);
        t->f_norm = x_font_cell(name);
        if (t->f_norm) {
            t->unicode = 1;
            snprintf(name, sizeof name, fams[i][1], px);
            t->f_bold = x_font_cell(name);
        }
    }
    if (!t->f_norm) {
        snprintf(name, sizeof name,
                 "-b&h-lucidatypewriter-medium-r-normal-sans-%d-*-*-*-m-*-iso8859-1", px);
        t->f_norm = x_font_cell(name);
        snprintf(name, sizeof name,
                 "-b&h-lucidatypewriter-bold-r-normal-sans-%d-*-*-*-m-*-iso8859-1", px);
        t->f_bold = t->f_norm ? x_font_cell(name) : 0;
    }
    if (!t->f_norm)
        t->f_norm = x_font_cell("fixed");
    if (t->f_bold && x_text_width(t->f_bold, "M") != x_text_width(t->f_norm, "M"))
        t->f_bold = 0;                  /* bold must fit the same cells */
    t->cw = x_text_width(t->f_norm, "M");
    t->asc = x_font_ascent(t->f_norm);
    t->ch = t->asc + x_font_descent(t->f_norm);
    if (t->cw < 4)
        t->cw = 7;
    if (t->ch < 8)
        t->ch = 13, t->asc = 11;
}

/* ---------- the program ---------- */

static inline void term_reply(void *ctx, const char *s, size_t n)
{
    struct term *t = ctx;

    while (t->ptm >= 0 && n > 0) {
        ssize_t w = write(t->ptm, s, n);

        if (w <= 0) {
            if (w < 0 && errno == EINTR)
                continue;
            return;
        }
        s += w;
        n -= (size_t)w;
    }
}

static inline void term_send(struct term *t, const char *s)
{
    term_reply(t, s, strlen(s));
}

static inline void term_winsize(struct term *t)
{
    struct winsize ws;

    if (t->ptm < 0)
        return;
    memset(&ws, 0, sizeof ws);
    ws.ws_row = (unsigned short)t->vt.rows;
    ws.ws_col = (unsigned short)t->vt.cols;
    ws.ws_xpixel = (unsigned short)(t->vt.cols * t->cw);
    ws.ws_ypixel = (unsigned short)(t->vt.rows * t->ch);
    ioctl(t->ptm, TIOCSWINSZ, &ws);     /* the program gets SIGWINCH */
}

static inline void term_rows_cols(const struct term *t, int *rows, int *cols)
{
    *cols = t->w / t->cw;
    *rows = t->h / t->ch;
    if (*cols < 10)
        *cols = 10;
    if (*rows < 3)
        *rows = 3;
}

/* Forget what the window shows (after it was drawn over): all of it is
 * drawn again next time. */
static inline void term_forget(struct term *t)
{
    int n = t->vt.rows * t->vt.cols;

    if (!t->started)
        return;
    free(t->shown);
    free(t->shown_hash);
    free(t->hash);
    free(t->tmp);
    t->shown_rows = t->vt.rows;
    t->shown_cols = t->vt.cols;
    t->shown = term_alloc((size_t)n * sizeof *t->shown);
    t->shown_hash = term_alloc((size_t)t->vt.rows * sizeof *t->shown_hash);
    t->hash = term_alloc((size_t)t->vt.rows * sizeof *t->hash);
    t->tmp = term_alloc((size_t)(t->vt.cols + 1) * sizeof *t->tmp);
    for (int i = 0; i < n; i++)
        t->shown[i].attr = 0xffffffffu;     /* matches nothing */
    t->shown_cx = t->shown_cy = -1;
    x_color(t->bg);
    x_fill_rect(t->x, t->y, t->w, t->h);
}

/* Put the terminal at (x, y), w x h pixels; it takes as many cells as fit
 * and tells the program its new size. */
static inline void term_place(struct term *t, int x, int y, int w, int h)
{
    int rows, cols;

    t->x = x;
    t->y = y;
    t->w = w;
    t->h = h;
    if (!t->started)
        return;
    term_rows_cols(t, &rows, &cols);
    if (rows != t->vt.rows || cols != t->vt.cols) {
        vt_resize(&t->vt, rows, cols);
        term_winsize(t);
    }
    t->view = 0;
    term_forget(t);
}

/* Start argv (NULL: $SHELL as a login shell) in dir (NULL: here), on a
 * new pseudo-terminal the size of the area. Any earlier program's screen
 * and history are cleared. Returns 0, or -1 with errno set. */
static inline int term_start(struct term *t, const char *dir, char *const argv[])
{
    char name[128];
    const char *sh = getenv("SHELL");
    int rows, cols, s, e;

    if (!sh || !*sh)
        sh = "/bin/sh";
    term_rows_cols(t, &rows, &cols);
    if (t->started)
        vt_free(&t->vt);
    vt_init(&t->vt, rows, cols, NULL, 0, 256 * 1024);
    t->vt.reply = term_reply;
    t->vt.reply_ctx = t;
    t->started = 1;
    t->view = 0;
    t->running = t->reaped = t->status = 0;
    term_forget(t);

    t->ptm = posix_openpt(O_RDWR | O_NOCTTY);
    if (t->ptm < 0)
        return -1;
    if (grantpt(t->ptm) != 0 || unlockpt(t->ptm) != 0 || ptsname(t->ptm) == NULL)
        goto fail;
    snprintf(name, sizeof name, "%s", ptsname(t->ptm));
    /* Open the program's side now: until something has it open, reading
     * ours fails (EIO) as if the program had already gone. */
    s = open(name, O_RDWR | O_NOCTTY);
    if (s < 0)
        goto fail;
    term_winsize(t);
    fflush(NULL);
    t->pid = fork();
    if (t->pid < 0) {
        close(s);
        goto fail;
    }
    if (t->pid == 0) {
        setsid();
#ifdef TIOCSCTTY
        ioctl(s, TIOCSCTTY, 0);
#endif
        dup2(s, 0);
        dup2(s, 1);
        dup2(s, 2);
        for (int fd = 3; fd < 256; fd++)
            close(fd);
        signal(SIGPIPE, SIG_DFL);       /* not what we ignore (or nohup did) */
        signal(SIGHUP, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        setenv("TERM", "screen-256color", 1);
        unsetenv("COLUMNS");
        unsetenv("LINES");
        if (dir && chdir(dir) != 0) {
            fprintf(stderr, "cd %s: %s\n", dir, strerror(errno));
            _exit(126);
        }
        if (argv && argv[0]) {
            execvp(argv[0], argv);
            fprintf(stderr, "%s: %s\n", argv[0], strerror(errno));
        } else {
            const char *b = strrchr(sh, '/');
            char argv0[64];

            snprintf(argv0, sizeof argv0, "-%s", b ? b + 1 : sh);   /* a login shell */
            execl(sh, argv0, (char *)NULL);
            fprintf(stderr, "%s: %s\n", sh, strerror(errno));
        }
        _exit(127);
    }
    close(s);
    fcntl(t->ptm, F_SETFL, fcntl(t->ptm, F_GETFL) | O_NONBLOCK);
    fcntl(t->ptm, F_SETFD, FD_CLOEXEC);
    t->running = 1;
    return 0;
fail:
    e = errno;
    close(t->ptm);
    t->ptm = -1;
    errno = e;
    return -1;
}

/* The fd to poll for output, or -1 once the program has gone. */
static inline int term_fd(const struct term *t)
{
    return t->running ? t->ptm : -1;
}

/* Has the program been waited for? Sets t->status (as from waitpid). */
static inline int term_reap(struct term *t)
{
    int st;

    if (!t->reaped && t->pid > 0 && waitpid(t->pid, &st, WNOHANG) == t->pid) {
        t->reaped = 1;
        t->status = st;
    }
    return t->reaped;
}

/* Read what the program wrote. Returns whether anything changed; when the
 * program has gone, t->running is 0 (and the pty is closed). */
static inline int term_pump(struct term *t)
{
    unsigned char buf[4096];
    int got = 0;

    if (!t->running)
        return 0;
    for (int i = 0; i < 16; i++) {      /* a burst, then draw */
        ssize_t r = read(t->ptm, buf, sizeof buf);

        if (r > 0) {
            vt_write(&t->vt, buf, (size_t)r);
            got = 1;
            t->view = 0;                /* new output: back to the bottom */
            continue;
        }
        if (r < 0 && (errno == EAGAIN || errno == EINTR))
            return got;
        break;                          /* EOF or EIO: nothing has it open */
    }
    if (got)
        return 1;
    close(t->ptm);
    t->ptm = -1;
    t->running = 0;
    if (!term_reap(t)) {                /* it's exiting: give it a moment */
        for (int i = 0; i < 20 && !term_reap(t); i++)
            usleep(5000);
    }
    return 1;
}

/* Is there nothing to see: a blank screen and no history? (What a
 * full-screen program such as vi or mc leaves when it ends.) */
static inline int term_blank(const struct term *t)
{
    if (!t->started || sb_count(&t->vt.sb) > 0)
        return 0;
    for (int i = 0; i < t->vt.rows * t->vt.cols; i++) {
        const struct cell *c = &t->vt.g->c[i];

        if ((c->ch && c->ch != ' ') || CELL_BG(c->attr) != COL_DEFAULT)
            return 0;
    }
    return 1;
}

/* Send a signal to the program's process group. */
static inline void term_signal(struct term *t, int sig)
{
    if (t->running && t->pid > 0)
        kill(-t->pid, sig);
}

/* ---------- drawing ---------- */

static inline unsigned term_row_hash(const struct cell *c, int n)
{
    unsigned h = 2166136261u;

    for (int i = 0; i < n; i++)
        h = (h ^ c[i].ch) * 16777619u ^ c[i].attr * 31u;
    return h;
}

/* The cells of screen row r as shown now, scrolled back by t->view. */
static inline const struct cell *term_row(struct term *t, int r)
{
    if (t->view > 0) {
        long n = sb_count(&t->vt.sb), i = n - t->view + r;

        if (i < n) {
            if (i < 0 || !sb_get(&t->vt.sb, i, t->tmp, t->vt.cols))
                for (int x = 0; x < t->vt.cols; x++)
                    t->tmp[x].ch = ' ', t->tmp[x].attr = ATTR_DEFAULT;
            return t->tmp;
        }
        r = (int)(i - n);
    }
    return vt_cell(&t->vt, r, 0);
}

/* Draw cells [x0, x1) of row y; cx is the cursor's column, or -1. */
static inline void term_draw_run(struct term *t, int y, const struct cell *c, int x0, int x1, int cx)
{
    unsigned int text[512];
    int py = t->y + y * t->ch;

    while (x0 < x1) {
        unsigned fg, bg;
        int x = x0, n = 0, fl, blank = 1, px = t->x + x0 * t->cw;

        term_colors(t, &c[x0], &fg, &bg);
        fl = (int)CELL_FL(c[x0].attr);
        if (x0 == cx) {                 /* the cursor: a block */
            fg = t->bg;
            bg = t->cursor;
        }
        /* a run of cells drawn the same way */
        while (x < x1 && n < 500) {
            unsigned f2, b2;

            if (x != x0) {
                if (x == cx || x0 == cx)
                    break;
                term_colors(t, &c[x], &f2, &b2);
                if ((int)CELL_FL(c[x].attr) != fl || f2 != fg || b2 != bg)
                    break;
            }
            if (c[x].attr & CELL_CONT) {
                x++;
                continue;
            }
            text[n] = c[x].ch ? c[x].ch : ' ';
            if (text[n] != ' ')
                blank = 0;
            n++;
            x += char_width(c[x].ch) == 2 && x + 1 < x1 && (c[x + 1].attr & CELL_CONT) ? 2 : 1;
        }
        x_color(bg);
        x_fill_rect(px, py, (x - x0) * t->cw, t->ch);
        if (!blank && !(fl & AT_HIDDEN)) {
            x_text_font(fl & AT_BOLD && t->f_bold ? t->f_bold : t->f_norm);
            x_color(fl & AT_DIM ? t->palette[8] : fg);
            if (t->unicode) {
                x_text16(px, py + t->asc, text, n);
            } else {
                char s[512];

                for (int i = 0; i < n; i++)
                    s[i] = (char)(text[i] < 256 ? text[i] : '?');
                s[n] = 0;
                x_text(px, py + t->asc, s);
            }
        }
        if (fl & (AT_UNDER | AT_STRIKE)) {
            x_color(fg);
            if (fl & AT_UNDER)
                x_fill_rect(px, py + t->asc + 1, (x - x0) * t->cw, 1);
            if (fl & AT_STRIKE)
                x_fill_rect(px, py + t->asc / 2 + 1, (x - x0) * t->cw, 1);
        }
        x0 = x;
    }
}

/* Draw what changed since the last call (into the back buffer). */
static inline void term_draw(struct term *t)
{
    int rows = t->vt.rows, cols = t->vt.cols, cx = -1, cy = -1;

    if (!t->started)
        return;
    if (t->shown == NULL || t->shown_rows != rows || t->shown_cols != cols)
        term_forget(t);
    if (t->focus && t->running && t->view == 0 && t->vt.cursor_visible) {
        cx = t->vt.x < cols ? t->vt.x : cols - 1;
        cy = t->vt.y;
    }

    /* Has the text moved up by k lines? Then move what's on screen first. */
    for (int r = 0; r < rows; r++)
        t->hash[r] = term_row_hash(term_row(t, r), cols);
    for (int k = 1; k < rows - 1; k++) {
        int same = 0, need = rows - k;

        for (int r = 0; r + k < rows; r++)
            same += t->hash[r] == t->shown_hash[r + k] && t->hash[r] != t->shown_hash[r];
        if (same >= 3 && same * 4 >= need * 3) {
            x_blit(x_frame(), t->x, t->y + k * t->ch, cols * t->cw, (rows - k) * t->ch, t->x, t->y);
            memmove(t->shown, t->shown + (size_t)k * cols,
                    (size_t)(rows - k) * cols * sizeof *t->shown);
            memmove(t->shown_hash, t->shown_hash + k, (size_t)(rows - k) * sizeof *t->shown_hash);
            for (int i = (rows - k) * cols; i < rows * cols; i++)
                t->shown[i].attr = 0xffffffffu;
            for (int r = rows - k; r < rows; r++)
                t->shown_hash[r] = 0;
            if (t->shown_cy >= 0)
                t->shown_cy -= k;
            break;
        }
    }
    /* where the cursor was must be drawn again */
    if (t->shown_cy >= 0 && t->shown_cy < rows && t->shown_cx >= 0 && t->shown_cx < cols &&
        (t->shown_cx != cx || t->shown_cy != cy))
        t->shown[t->shown_cy * cols + t->shown_cx].attr = 0xffffffffu;

    for (int r = 0; r < rows; r++) {
        const struct cell *c;
        struct cell *s = t->shown + (size_t)r * cols;
        int x0 = -1, x1 = -1;

        if (t->hash[r] == t->shown_hash[r] && r != cy && r != t->shown_cy)
            continue;
        c = term_row(t, r);
        for (int x = 0; x < cols; x++)
            if (c[x].ch != s[x].ch || c[x].attr != s[x].attr || (r == cy && x == cx)) {
                if (x0 < 0)
                    x0 = x;
                x1 = x + 1;
            }
        if (x0 >= 0) {
            if (x0 > 0 && (c[x0].attr & CELL_CONT))
                x0--;                   /* start at the whole wide char */
            term_draw_run(t, r, c, x0, x1, r == cy ? cx : -1);
            memcpy(s + x0, c + x0, (size_t)(x1 - x0) * sizeof *s);
        }
        t->shown_hash[r] = t->hash[r];
    }
    t->shown_cx = cx;
    t->shown_cy = cy;
    if (cy >= 0)
        t->shown[cy * cols + cx].attr = 0xfffffffeu;    /* redraw it when the cursor goes */
    if (t->view > 0) {                  /* how far back, top right */
        char s[48];
        int w, bx;

        snprintf(s, sizeof s, " %d lines back ", t->view);
        w = (int)strlen(s) * t->cw;
        bx = t->x + cols * t->cw - w;
        x_color(t->cursor);
        x_fill_rect(bx, t->y, w, t->ch);
        x_text_font(t->f_norm);
        x_color(t->bg);
        x_text(bx, t->y + t->asc, s);
        for (int x = cols - (int)strlen(s); x < cols; x++)
            if (x >= 0)
                t->shown[x].attr = 0xffffffffu;
        t->shown_hash[0] = 0;
    }
}

/* ---------- input ---------- */

/* Scroll the history view by d lines (up is positive); whether it moved. */
static inline int term_scroll(struct term *t, int d)
{
    long n = t->started ? sb_count(&t->vt.sb) : 0;
    int old = t->view;

    t->view += d;
    if (t->view > n)
        t->view = (int)n;
    if (t->view < 0)
        t->view = 0;
    return t->view != old;
}

/* A key, as screen-256color's terminfo says it arrives. Returns whether
 * the view moved (Shift-PgUp/PgDn scroll the history). */
static inline int term_key(struct term *t, const struct x_event *e)
{
    int k = e->key, m = e->mods;
    int mod = 1 + ((m & X_MOD_SHIFT) ? 1 : 0) + ((m & X_MOD_ALT) ? 2 : 0) + ((m & X_MOD_CTRL) ? 4 : 0);
    char s[32];
    const char *seq = NULL;

    if ((m & X_MOD_SHIFT) && (k == X_KEY_PAGE_UP || k == X_KEY_PAGE_DOWN))
        return term_scroll(t, (k == X_KEY_PAGE_UP ? 1 : -1) * (t->vt.rows - 1));
    if (!t->running)
        return 0;
    switch (k) {
    case X_KEY_UP: case X_KEY_DOWN: case X_KEY_RIGHT: case X_KEY_LEFT: {
        char d = k == X_KEY_UP ? 'A' : k == X_KEY_DOWN ? 'B' : k == X_KEY_RIGHT ? 'C' : 'D';

        if (mod > 1)
            snprintf(s, sizeof s, "\033[1;%d%c", mod, d);
        else
            snprintf(s, sizeof s, t->vt.app_cursor ? "\033O%c" : "\033[%c", d);
        seq = s;
        m &= ~X_MOD_ALT;                /* already in the sequence */
        break;
    }
    case X_KEY_HOME: seq = "\033[1~"; break;
    case X_KEY_END: seq = "\033[4~"; break;
    case X_KEY_INSERT: seq = "\033[2~"; break;
    case X_KEY_DELETE: seq = "\033[3~"; break;
    case X_KEY_PAGE_UP: seq = "\033[5~"; break;
    case X_KEY_PAGE_DOWN: seq = "\033[6~"; break;
    case X_KEY_RETURN: case X_KEY_KP_ENTER: seq = "\r"; break;
    case X_KEY_BACKSPACE: seq = (m & X_MOD_CTRL) ? "\010" : "\177"; break;
    case X_KEY_TAB: seq = (m & X_MOD_SHIFT) ? "\033[Z" : "\t"; break;
    case X_KEY_BACKTAB: seq = "\033[Z"; break;
    case X_KEY_ESCAPE: seq = "\033"; break;
    default:
        if (k >= X_KEY_F1 && k < X_KEY_F1 + 12) {
            static const char *const fk[12] = {
                "\033OP", "\033OQ", "\033OR", "\033OS", "\033[15~", "\033[17~",
                "\033[18~", "\033[19~", "\033[20~", "\033[21~", "\033[23~", "\033[24~",
            };

            seq = fk[k - X_KEY_F1];
        } else if (k > 0 && k < 256) {
            int n = 0;

            if (m & X_MOD_ALT)
                s[n++] = '\033';
            if (m & X_MOD_CTRL) {
                if (k >= '@' && k <= '~')
                    k &= 0x1f;
                else if (k == ' ' || k == '2')
                    k = 0;
                else if (k >= '3' && k <= '7')
                    k = k - '3' + 27;
                else if (k == '8')
                    k = 127;
                else if (k == '/')
                    k = 31;
            }
            if (k < 128) {
                s[n++] = (char)k;
            } else {                    /* Latin-1 as UTF-8 */
                s[n++] = (char)(0xC0 | (k >> 6));
                s[n++] = (char)(0x80 | (k & 0x3F));
            }
            term_reply(t, s, (size_t)n);
            return term_scroll(t, -t->view);
        }
        break;
    }
    if (seq == NULL)
        return 0;                       /* Shift, Ctrl and other keys alone */
    if (m & X_MOD_ALT)
        term_send(t, "\033");
    term_send(t, seq);
    return term_scroll(t, -t->view);    /* typing: back to the bottom */
}

/* A mouse report, for a program that asked for them. */
static inline void term_mouse_report(struct term *t, int b, int x, int y, int release, int motion)
{
    char s[48];

    x = (x - t->x) / t->cw + 1;
    y = (y - t->y) / t->ch + 1;
    if (x < 1) x = 1;
    if (y < 1) y = 1;
    if (x > t->vt.cols) x = t->vt.cols;
    if (y > t->vt.rows) y = t->vt.rows;
    if (motion)
        b += 32;
    if (t->vt.mouse & MOUSE_SGR) {
        snprintf(s, sizeof s, "\033[<%d;%d;%d%c", b, x, y, release ? 'm' : 'M');
    } else {
        if (release)
            b = 3;
        if (x > 223) x = 223;
        if (y > 223) y = 223;
        snprintf(s, sizeof s, "\033[M%c%c%c", 32 + b, 32 + x, 32 + y);
    }
    term_send(t, s);
}

/* A button or motion event (in window coordinates). Returns whether the
 * view moved. */
static inline int term_mouse(struct term *t, const struct x_event *e)
{
    int b, app = t->running && t->vt.mouse && !(e->mods & X_MOD_SHIFT);

    if (e->type == XE_BUTTON_DOWN && (e->button == 4 || e->button == 5) && !app) {
        if (t->running && t->vt.g == &t->vt.alt && !(e->mods & X_MOD_SHIFT)) {
            const char *up = t->vt.app_cursor ? "\033OA" : "\033[A";
            const char *dn = t->vt.app_cursor ? "\033OB" : "\033[B";

            for (int i = 0; i < 3; i++)     /* less, man: the wheel is arrows */
                term_send(t, e->button == 4 ? up : dn);
            return 0;
        }
        return term_scroll(t, e->button == 4 ? 3 : -3);
    }
    if (!app)
        return 0;
    if (e->type == XE_MOTION) {
        if ((t->held >= 0 && (t->vt.mouse & (MOUSE_DRAG | MOUSE_ANY))) || (t->vt.mouse & MOUSE_ANY))
            term_mouse_report(t, t->held >= 0 ? t->held : 3, e->x, e->y, 0, 1);
        return 0;
    }
    b = e->button == 1 ? 0 : e->button == 2 ? 1 : e->button == 3 ? 2 : e->button == 4 ? 64 : 65;
    if (e->type == XE_BUTTON_DOWN) {
        if (b < 3)
            t->held = b;
        term_mouse_report(t, b, e->x, e->y, 0, 0);
    } else if (b < 3) {
        t->held = -1;
        term_mouse_report(t, b, e->x, e->y, 1, 0);
    }
    return 0;
}

/* Hang up on the program, if it's still there. */
static inline void term_close(struct term *t)
{
    term_signal(t, SIGHUP);
    if (t->ptm >= 0)
        close(t->ptm);
    t->ptm = -1;
    t->running = 0;
}

/* Hang up and free the screen and history; the fonts and colours stay,
 * so term_start can use it again. */
static inline void term_free(struct term *t)
{
    term_close(t);
    if (t->started)
        vt_free(&t->vt);
    free(t->shown);
    free(t->shown_hash);
    free(t->hash);
    free(t->tmp);
    t->shown = t->tmp = NULL;
    t->shown_hash = t->hash = NULL;
    t->started = 0;
}

#endif /* TERM_H */
