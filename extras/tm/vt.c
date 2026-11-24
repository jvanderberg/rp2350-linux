/*
 * vt.c - terminal emulator. See vt.h for what it supports.
 *
 * The parser is a small state machine in the spirit of the DEC/vt100
 * one: GROUND prints characters and executes C0 controls; ESC, CSI, OSC
 * and string states collect a sequence and dispatch it when complete.
 * Sequences are recognised byte by byte, so they may be split across
 * reads.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "vt.h"

enum {
    S_GROUND,
    S_ESC,          /* after ESC */
    S_ESC_SKIP,     /* ESC + intermediate: skip the final byte */
    S_ESC_HASH,     /* ESC # */
    S_CHARSET,      /* ESC ( or ESC ): the next byte names the charset */
    S_CSI,          /* ESC [ ... */
    S_OSC,          /* ESC ] ... until BEL or ST */
    S_STR,          /* DCS/SOS/PM/APC ... until ST (ignored) */
    S_STR_ESC       /* ESC seen inside OSC or a string: ST if '\' */
};

/* DEC special graphics, for characters 0x5f..0x7e. */
static const unsigned short dec_graphics[32] = {
    0x0020, 0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0,
    0x00B1, 0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C,
    0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534,
    0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7
};

/* ---- helpers ---------------------------------------------------------- */

static struct cell blank(const struct vt *vt)
{
    struct cell c;

    /* erase with the current background (xterm's "bce" behaviour) */
    c.ch = ' ';
    c.attr = MKATTR(COL_DEFAULT, CELL_BG(vt->attr), 0);
    return c;
}

/* Every change to the screen goes through here (or touch()), so this is
 * where rows are marked as needing a redraw. */
static struct cell *row_of(struct vt *vt, int y)
{
    vt->rowdirty[y] = 1;
    return &vt->g->c[y * vt->cols];
}

static void touch(struct vt *vt, int y0, int y1)
{
    if (y0 <= y1)
        memset(vt->rowdirty + y0, 1, (size_t)(y1 - y0 + 1));
}

static void clear_cells(struct vt *vt, int y, int x0, int x1)
{
    struct cell *r = row_of(vt, y), b = blank(vt);
    int x;

    if (x0 < 0)
        x0 = 0;
    if (x1 > vt->cols)
        x1 = vt->cols;
    if (x0 >= x1)
        return;
    /* don't leave half of a wide character behind */
    if ((r[x0].attr & CELL_CONT) && x0 > 0)
        r[x0 - 1] = b;
    if (x1 < vt->cols && (r[x1].attr & CELL_CONT))
        r[x1] = b;
    for (x = x0; x < x1; x++)
        r[x] = b;
}

static void clear_rows(struct vt *vt, int y0, int y1)
{
    int y;

    for (y = y0; y < y1; y++)
        clear_cells(vt, y, 0, vt->cols);
}

static void reply(struct vt *vt, const char *s)
{
    if (vt->reply != NULL)
        vt->reply(vt->reply_ctx, s, strlen(s));
}

static int param(const struct vt *vt, int i, int def)
{
    return i < vt->nparams && vt->params[i] > 0 ? vt->params[i] : def;
}

static void reset_tabs(struct vt *vt)
{
    int x;

    for (x = 0; x < vt->cols; x++)
        vt->tabs[x] = x > 0 && x % 8 == 0;
}

/* ---- scrolling -------------------------------------------------------- */

/* Scroll rows [top, bot] up by n. With `history`, rows leaving the top of
 * the main screen are saved to the scrollback. */
static void scroll_up(struct vt *vt, int top, int bot, int n, int history)
{
    int y, h = bot - top + 1;

    if (n > h)
        n = h;
    if (n <= 0)
        return;
    touch(vt, top, bot);
    if (history && vt->g == &vt->main && top == 0)
        for (y = 0; y < n; y++)
            sb_push(&vt->sb, row_of(vt, y), vt->cols);
    memmove(row_of(vt, top), row_of(vt, top + n),
            sizeof(struct cell) * (size_t)vt->cols * (size_t)(h - n));
    clear_rows(vt, bot - n + 1, bot + 1);
}

static void scroll_down(struct vt *vt, int top, int bot, int n)
{
    int h = bot - top + 1;

    if (n > h)
        n = h;
    if (n <= 0)
        return;
    touch(vt, top, bot);
    memmove(row_of(vt, top + n), row_of(vt, top),
            sizeof(struct cell) * (size_t)vt->cols * (size_t)(h - n));
    clear_rows(vt, top, top + n);
}

static void linefeed(struct vt *vt)
{
    vt->wrapnext = 0;
    if (vt->y == vt->bot)
        scroll_up(vt, vt->top, vt->bot, 1, 1);
    else if (vt->y < vt->rows - 1)
        vt->y++;
}

static void reverse_index(struct vt *vt)
{
    vt->wrapnext = 0;
    if (vt->y == vt->top)
        scroll_down(vt, vt->top, vt->bot, 1);
    else if (vt->y > 0)
        vt->y--;
}

/* ---- cursor ----------------------------------------------------------- */

static void move_to(struct vt *vt, int x, int y)
{
    int lo = vt->origin ? vt->top : 0, hi = vt->origin ? vt->bot : vt->rows - 1;

    if (vt->origin)
        y += vt->top;
    vt->x = x < 0 ? 0 : x >= vt->cols ? vt->cols - 1 : x;
    vt->y = y < lo ? lo : y > hi ? hi : y;
    vt->wrapnext = 0;
}

static void save_cursor(struct vt *vt)
{
    struct cursor_save *s = vt->g == &vt->alt ? &vt->saved_alt : &vt->saved;

    s->x = vt->x;
    s->y = vt->y;
    s->attr = vt->attr;
    s->origin = vt->origin;
    s->gl = vt->gl;
    s->g[0] = vt->charset[0];
    s->g[1] = vt->charset[1];
}

static void restore_cursor(struct vt *vt)
{
    struct cursor_save *s = vt->g == &vt->alt ? &vt->saved_alt : &vt->saved;

    vt->x = s->x < vt->cols ? s->x : vt->cols - 1;
    vt->y = s->y < vt->rows ? s->y : vt->rows - 1;
    vt->attr = s->attr;
    vt->origin = s->origin;
    vt->gl = s->gl;
    vt->charset[0] = s->g[0] ? s->g[0] : 'B';
    vt->charset[1] = s->g[1] ? s->g[1] : 'B';
    vt->wrapnext = 0;
}

/* ---- screens ---------------------------------------------------------- */

static void alloc_grid(struct grid *g, int rows, int cols)
{
    size_t n = (size_t)rows * (size_t)cols, i;

    g->c = xmalloc(sizeof *g->c * n);
    for (i = 0; i < n; i++) {
        g->c[i].ch = ' ';
        g->c[i].attr = ATTR_DEFAULT;
    }
}

static void use_alt(struct vt *vt, int on, int clear)
{
    if (on && vt->g != &vt->alt) {
        if (vt->alt.c == NULL)          /* allocated on first use */
            alloc_grid(&vt->alt, vt->rows, vt->cols);
        vt->g = &vt->alt;
        if (clear)
            clear_rows(vt, 0, vt->rows);
    } else if (!on && vt->g == &vt->alt) {
        if (clear)
            clear_rows(vt, 0, vt->rows);
        vt->g = &vt->main;
    }
    touch(vt, 0, vt->rows - 1);
    vt->wrapnext = 0;
}

static void full_reset(struct vt *vt)
{
    use_alt(vt, 0, 0);
    vt->attr = ATTR_DEFAULT;
    vt->x = vt->y = 0;
    vt->wrapnext = 0;
    vt->top = 0;
    vt->bot = vt->rows - 1;
    vt->autowrap = 1;
    vt->origin = vt->insert = 0;
    vt->cursor_visible = 1;
    vt->app_cursor = vt->app_keypad = vt->mouse = vt->bracketed_paste = 0;
    vt->charset[0] = vt->charset[1] = 'B';
    vt->gl = 0;
    memset(&vt->saved, 0, sizeof vt->saved);
    memset(&vt->saved_alt, 0, sizeof vt->saved_alt);
    vt->saved.attr = vt->saved_alt.attr = ATTR_DEFAULT;
    reset_tabs(vt);
    clear_rows(vt, 0, vt->rows);
}

void vt_init(struct vt *vt, int rows, int cols, const char *history_dir,
             size_t disk_bytes, size_t ram_bytes)
{
    memset(vt, 0, sizeof *vt);
    vt->rows = rows;
    vt->cols = cols;
    alloc_grid(&vt->main, rows, cols);
    vt->g = &vt->main;
    vt->rowdirty = xmalloc((size_t)rows);
    vt->tabs = xmalloc((size_t)cols);
    sb_init(&vt->sb, history_dir, disk_bytes, ram_bytes);
    full_reset(vt);
}

void vt_free(struct vt *vt)
{
    free(vt->main.c);
    free(vt->alt.c);
    free(vt->tabs);
    free(vt->rowdirty);
    sb_free(&vt->sb);
}

/* ---- printing --------------------------------------------------------- */

static void put_char(struct vt *vt, uint32_t cp)
{
    struct cell *r;
    int w;

    if (vt->charset[vt->gl] == '0' && cp >= 0x5F && cp <= 0x7E)
        cp = dec_graphics[cp - 0x5F];
    w = char_width(cp);
    if (w == 0)
        return;                     /* combining marks are dropped */
    if (vt->wrapnext && vt->autowrap) {
        vt->x = 0;
        linefeed(vt);
    }
    vt->wrapnext = 0;
    if (w == 2 && vt->x == vt->cols - 1) {
        if (!vt->autowrap)
            return;
        clear_cells(vt, vt->y, vt->x, vt->cols);
        vt->x = 0;
        linefeed(vt);
    }
    r = row_of(vt, vt->y);
    if (vt->insert)
        memmove(r + vt->x + w, r + vt->x,
                sizeof *r * (size_t)(vt->cols - vt->x - w));
    /* overwriting half of a wide character blanks the other half */
    if ((r[vt->x].attr & CELL_CONT) && vt->x > 0)
        r[vt->x - 1] = blank(vt);
    if (vt->x + w < vt->cols && (r[vt->x + w].attr & CELL_CONT))
        r[vt->x + w] = blank(vt);
    r[vt->x].ch = cp;
    r[vt->x].attr = vt->attr;
    if (w == 2) {
        r[vt->x + 1].ch = ' ';
        r[vt->x + 1].attr = vt->attr | CELL_CONT;
    }
    vt->last_char = cp;
    vt->x += w;
    if (vt->x >= vt->cols) {
        vt->x = vt->cols - 1;
        vt->wrapnext = vt->autowrap;
    }
}

static void control(struct vt *vt, unsigned char c)
{
    switch (c) {
    case 7:
        vt->bell = 1;
        break;
    case 8:
        if (vt->x > 0)
            vt->x--;
        vt->wrapnext = 0;
        break;
    case 9:
        while (vt->x < vt->cols - 1 && !vt->tabs[++vt->x])
            ;
        vt->wrapnext = 0;
        break;
    case 10: case 11: case 12:
        linefeed(vt);
        break;
    case 13:
        vt->x = 0;
        vt->wrapnext = 0;
        break;
    case 14:
        vt->gl = 1;
        break;
    case 15:
        vt->gl = 0;
        break;
    }
}

/* ---- SGR -------------------------------------------------------------- */

/* Nearest xterm-256 colour of an RGB value (the 6x6x6 cube). */
static int rgb_to_256(int r, int g, int b)
{
    int q[3], i, v[3];

    v[0] = r;
    v[1] = g;
    v[2] = b;
    for (i = 0; i < 3; i++)
        q[i] = v[i] < 48 ? 0 : v[i] < 115 ? 1 : (v[i] - 35) / 40;
    return 16 + 36 * q[0] + 6 * q[1] + q[2];
}

static void sgr(struct vt *vt)
{
    uint32_t fg = CELL_FG(vt->attr), bg = CELL_BG(vt->attr);
    uint32_t fl = CELL_FL(vt->attr);
    int i;

    if (vt->nparams == 0) {
        vt->attr = ATTR_DEFAULT;
        return;
    }
    for (i = 0; i < vt->nparams; i++) {
        int p = vt->params[i];

        if (p == 38 || p == 48) {       /* extended colour */
            int c = -1;

            if (i + 2 < vt->nparams && vt->params[i + 1] == 5) {
                c = vt->params[i + 2] & 255;
                i += 2;
            } else if (i + 4 < vt->nparams && vt->params[i + 1] == 2) {
                c = rgb_to_256(vt->params[i + 2], vt->params[i + 3],
                               vt->params[i + 4]);
                i += 4;
            }
            if (c >= 0) {
                if (p == 38)
                    fg = (uint32_t)c;
                else
                    bg = (uint32_t)c;
            }
            continue;
        }
        switch (p) {
        case 0:  fg = bg = COL_DEFAULT; fl = 0; break;
        case 1:  fl |= AT_BOLD; break;
        case 2:  fl |= AT_DIM; break;
        case 3:  fl |= AT_ITALIC; break;
        case 4:  fl |= AT_UNDER; break;
        case 5:  fl |= AT_BLINK; break;
        case 7:  fl |= AT_REVERSE; break;
        case 8:  fl |= AT_HIDDEN; break;
        case 9:  fl |= AT_STRIKE; break;
        case 21: case 22: fl &= ~(uint32_t)(AT_BOLD | AT_DIM); break;
        case 23: fl &= ~(uint32_t)AT_ITALIC; break;
        case 24: fl &= ~(uint32_t)AT_UNDER; break;
        case 25: fl &= ~(uint32_t)AT_BLINK; break;
        case 27: fl &= ~(uint32_t)AT_REVERSE; break;
        case 28: fl &= ~(uint32_t)AT_HIDDEN; break;
        case 29: fl &= ~(uint32_t)AT_STRIKE; break;
        case 39: fg = COL_DEFAULT; break;
        case 49: bg = COL_DEFAULT; break;
        default:
            if (p >= 30 && p <= 37)
                fg = (uint32_t)(p - 30);
            else if (p >= 40 && p <= 47)
                bg = (uint32_t)(p - 40);
            else if (p >= 90 && p <= 97)
                fg = (uint32_t)(p - 90 + 8);
            else if (p >= 100 && p <= 107)
                bg = (uint32_t)(p - 100 + 8);
            break;
        }
    }
    vt->attr = MKATTR(fg, bg, fl);
}

/* ---- modes ------------------------------------------------------------ */

static void set_mode(struct vt *vt, int on)
{
    int i;

    for (i = 0; i < (vt->nparams ? vt->nparams : 1); i++) {
        int p = vt->nparams ? vt->params[i] : 0;

        if (vt->priv == '?') {
            switch (p) {
            case 1:    vt->app_cursor = on; break;
            case 6:    vt->origin = on; move_to(vt, 0, 0); break;
            case 7:    vt->autowrap = on; break;
            case 25:   vt->cursor_visible = on; break;
            case 47:   use_alt(vt, on, 0); break;
            case 1047: use_alt(vt, on, !on); break;
            case 1048: if (on) save_cursor(vt); else restore_cursor(vt); break;
            case 1049:
                if (on) {
                    save_cursor(vt);
                    use_alt(vt, 1, 1);
                } else {
                    use_alt(vt, 0, 0);
                    restore_cursor(vt);
                }
                break;
            case 1000: vt->mouse = on ? (vt->mouse | MOUSE_CLICK)
                                      : (vt->mouse & ~MOUSE_CLICK); break;
            case 1002: vt->mouse = on ? (vt->mouse | MOUSE_DRAG)
                                      : (vt->mouse & ~MOUSE_DRAG); break;
            case 1003: vt->mouse = on ? (vt->mouse | MOUSE_ANY)
                                      : (vt->mouse & ~MOUSE_ANY); break;
            case 1006: vt->mouse = on ? (vt->mouse | MOUSE_SGR)
                                      : (vt->mouse & ~MOUSE_SGR); break;
            case 2004: vt->bracketed_paste = on; break;
            }
        } else if (vt->priv == 0 && p == 4) {
            vt->insert = on;
        }
    }
}

/* ---- CSI -------------------------------------------------------------- */

static void csi(struct vt *vt, unsigned char f)
{
    int n = param(vt, 0, 1), i;
    struct cell *r;
    char buf[48];

    if (vt->priv == '>' || vt->priv == '=') {
        if (f == 'c' && vt->priv == '>')
            reply(vt, "\x1b[>83;40003;0c");     /* like GNU screen */
        return;
    }
    if (vt->inter != 0) {
        if (vt->inter == '!' && f == 'p')       /* DECSTR soft reset */
            full_reset(vt);
        return;                                 /* DECSCUSR etc. */
    }
    if (vt->priv == '?' && f != 'h' && f != 'l')
        return;

    switch (f) {
    case 'A': move_to(vt, vt->x, vt->y - n - (vt->origin ? vt->top : 0)); break;
    case 'B': case 'e':
        move_to(vt, vt->x, vt->y + n - (vt->origin ? vt->top : 0)); break;
    case 'C': case 'a': move_to(vt, vt->x + n, vt->y - (vt->origin ? vt->top : 0)); break;
    case 'D': move_to(vt, vt->x - n, vt->y - (vt->origin ? vt->top : 0)); break;
    case 'E': move_to(vt, 0, vt->y + n - (vt->origin ? vt->top : 0)); break;
    case 'F': move_to(vt, 0, vt->y - n - (vt->origin ? vt->top : 0)); break;
    case 'G': case '`': move_to(vt, n - 1, vt->y - (vt->origin ? vt->top : 0)); break;
    case 'H': case 'f': move_to(vt, param(vt, 1, 1) - 1, param(vt, 0, 1) - 1); break;
    case 'd': move_to(vt, vt->x, n - 1); break;
    case 'I':
        while (n-- > 0)
            control(vt, 9);
        break;
    case 'Z':
        while (n-- > 0 && vt->x > 0)
            while (--vt->x > 0 && !vt->tabs[vt->x])
                ;
        break;
    case 'J': {
        int mode = vt->nparams ? vt->params[0] : 0;

        if (mode == 0) {
            clear_cells(vt, vt->y, vt->x, vt->cols);
            clear_rows(vt, vt->y + 1, vt->rows);
        } else if (mode == 1) {
            clear_rows(vt, 0, vt->y);
            clear_cells(vt, vt->y, 0, vt->x + 1);
        } else if (mode == 2) {
            clear_rows(vt, 0, vt->rows);
        } else if (mode == 3) {
            sb_clear(&vt->sb);
        }
        break;
    }
    case 'K': {
        int mode = vt->nparams ? vt->params[0] : 0;

        if (mode == 0)
            clear_cells(vt, vt->y, vt->x, vt->cols);
        else if (mode == 1)
            clear_cells(vt, vt->y, 0, vt->x + 1);
        else if (mode == 2)
            clear_cells(vt, vt->y, 0, vt->cols);
        break;
    }
    case 'L':
        if (vt->y >= vt->top && vt->y <= vt->bot)
            scroll_down(vt, vt->y, vt->bot, n);
        vt->x = 0;
        break;
    case 'M':
        if (vt->y >= vt->top && vt->y <= vt->bot)
            scroll_up(vt, vt->y, vt->bot, n, 0);
        vt->x = 0;
        break;
    case '@':
        r = row_of(vt, vt->y);
        if (n > vt->cols - vt->x)
            n = vt->cols - vt->x;
        memmove(r + vt->x + n, r + vt->x,
                sizeof *r * (size_t)(vt->cols - vt->x - n));
        clear_cells(vt, vt->y, vt->x, vt->x + n);
        break;
    case 'P':
        r = row_of(vt, vt->y);
        if (n > vt->cols - vt->x)
            n = vt->cols - vt->x;
        memmove(r + vt->x, r + vt->x + n,
                sizeof *r * (size_t)(vt->cols - vt->x - n));
        clear_cells(vt, vt->y, vt->cols - n, vt->cols);
        break;
    case 'X':
        clear_cells(vt, vt->y, vt->x, vt->x + n);
        break;
    case 'S':
        scroll_up(vt, vt->top, vt->bot, n, 0);
        break;
    case 'T':
        scroll_down(vt, vt->top, vt->bot, n);
        break;
    case 'b':
        for (i = 0; i < n && i < 4096 && vt->last_char; i++)
            put_char(vt, vt->last_char);
        break;
    case 'c':
        if (param(vt, 0, 0) == 0)
            reply(vt, "\x1b[?1;2c");
        break;
    case 'g':
        if (param(vt, 0, 0) == 0)
            vt->tabs[vt->x] = 0;
        else if (vt->params[0] == 3)
            memset(vt->tabs, 0, (size_t)vt->cols);
        break;
    case 'h':
        set_mode(vt, 1);
        break;
    case 'l':
        set_mode(vt, 0);
        break;
    case 'm':
        sgr(vt);
        break;
    case 'n':
        if (param(vt, 0, 0) == 5) {
            reply(vt, "\x1b[0n");
        } else if (param(vt, 0, 0) == 6) {
            sprintf(buf, "\x1b[%d;%dR",
                    vt->y + 1 - (vt->origin ? vt->top : 0), vt->x + 1);
            reply(vt, buf);
        }
        break;
    case 'r': {
        int top = param(vt, 0, 1) - 1, bot = param(vt, 1, vt->rows) - 1;

        if (bot >= vt->rows)
            bot = vt->rows - 1;
        if (top < bot) {
            vt->top = top;
            vt->bot = bot;
            move_to(vt, 0, 0);
        }
        break;
    }
    case 's':
        save_cursor(vt);
        break;
    case 'u':
        restore_cursor(vt);
        break;
    }
}

/* ---- ESC -------------------------------------------------------------- */

static void esc(struct vt *vt, unsigned char c)
{
    vt->state = S_GROUND;
    switch (c) {
    case '[':
        vt->state = S_CSI;
        vt->nparams = 0;
        vt->have_param = 0;
        vt->params[0] = 0;
        vt->priv = 0;
        vt->inter = 0;
        break;
    case ']':
        vt->state = S_OSC;
        break;
    case 'P': case 'X': case '^': case '_':
        vt->state = S_STR;
        break;
    case '(': case ')':
        vt->inter = (char)c;
        vt->state = S_CHARSET;
        break;
    case '*': case '+': case ' ': case '%':
        vt->state = S_ESC_SKIP;
        break;
    case '#':
        vt->state = S_ESC_HASH;
        break;
    case '7': save_cursor(vt); break;
    case '8': restore_cursor(vt); break;
    case 'D': linefeed(vt); break;
    case 'E': vt->x = 0; linefeed(vt); break;
    case 'H': vt->tabs[vt->x] = 1; break;
    case 'M': reverse_index(vt); break;
    case 'c': full_reset(vt); break;
    case '=': vt->app_keypad = 1; break;
    case '>': vt->app_keypad = 0; break;
    }
}

/* ---- the byte loop ---------------------------------------------------- */

static void ground(struct vt *vt, unsigned char c)
{
    if (vt->utf8_len > 0 || c >= 0x80) {
        uint32_t cp;
        int used;

        if (vt->utf8_len > 0 && (c & 0xC0) != 0x80) {
            vt->utf8_len = 0;           /* broken sequence */
            put_char(vt, 0xFFFD);
            if (c < 0x80) {
                ground(vt, c);
                return;
            }
        }
        vt->utf8[vt->utf8_len++] = c;
        cp = utf8_decode(vt->utf8, (size_t)vt->utf8_len, &used);
        if (used == 0 && vt->utf8_len < 4)
            return;                     /* need more bytes */
        vt->utf8_len = 0;
        put_char(vt, used == 0 ? 0xFFFD : cp);
        return;
    }
    if (c < 0x20) {
        if (c == 27)
            vt->state = S_ESC;
        else
            control(vt, c);
        return;
    }
    if (c == 0x7F)
        return;
    put_char(vt, c);
}

void vt_write(struct vt *vt, const unsigned char *buf, size_t n)
{
    size_t i;

    vt->dirty = 1;
    for (i = 0; i < n; i++) {
        unsigned char c = buf[i];

        switch (vt->state) {
        case S_GROUND:
            ground(vt, c);
            break;
        case S_ESC:
            esc(vt, c);
            break;
        case S_ESC_SKIP:
            vt->state = S_GROUND;
            break;
        case S_ESC_HASH:
            if (c == '8') {             /* DECALN: fill with 'E' */
                int k;

                for (k = 0; k < vt->rows * vt->cols; k++) {
                    vt->g->c[k].ch = 'E';
                    vt->g->c[k].attr = ATTR_DEFAULT;
                }
                touch(vt, 0, vt->rows - 1);
            }
            vt->state = S_GROUND;
            break;
        case S_CHARSET:
            vt->charset[vt->inter == ')'] = c == '0' ? '0' : 'B';
            vt->inter = 0;
            vt->state = S_GROUND;
            break;
        case S_CSI:
            if (c >= '0' && c <= '9') {
                if (vt->nparams == 0)
                    vt->nparams = 1;
                if (vt->params[vt->nparams - 1] < 100000)
                    vt->params[vt->nparams - 1] =
                        vt->params[vt->nparams - 1] * 10 + (c - '0');
            } else if (c == ';' || c == ':') {
                if (vt->nparams == 0)
                    vt->nparams = 1;
                if (vt->nparams < 16)
                    vt->params[vt->nparams++] = 0;
            } else if (c >= '<' && c <= '?') {
                vt->priv = (char)c;
            } else if (c >= 0x20 && c <= 0x2F) {
                vt->inter = (char)c;
            } else if (c >= 0x40 && c <= 0x7E) {
                vt->state = S_GROUND;
                csi(vt, c);
                vt->inter = 0;
            } else if (c == 27) {
                vt->state = S_ESC;
            } else if (c == 0x18 || c == 0x1A) {
                vt->state = S_GROUND;
            } else if (c < 0x20) {
                control(vt, c);         /* C0 inside CSI still acts */
            }
            break;
        case S_OSC:
        case S_STR:
            if (c == 7 && vt->state == S_OSC)
                vt->state = S_GROUND;
            else if (c == 27)
                vt->state = S_STR_ESC;
            else if (c == 0x18 || c == 0x1A)
                vt->state = S_GROUND;
            break;
        case S_STR_ESC:
            vt->state = S_GROUND;
            if (c != '\\')
                esc(vt, c);
            break;
        }
    }
}

/* ---- resize ----------------------------------------------------------- */

static void resize_grid(struct vt *vt, struct grid *g, int rows, int cols,
                        int shift)
{
    struct grid ng;
    int y, x;

    alloc_grid(&ng, rows, cols);
    for (y = 0; y < rows && y + shift < vt->rows; y++) {
        struct cell *src = &g->c[(y + shift) * vt->cols];

        for (x = 0; x < cols && x < vt->cols; x++)
            ng.c[y * cols + x] = src[x];
        /* a wide character cut in half at the new right edge */
        if (cols < vt->cols && cols > 0 &&
            (src[cols].attr & CELL_CONT)) {
            ng.c[y * cols + cols - 1].ch = ' ';
            ng.c[y * cols + cols - 1].attr = ATTR_DEFAULT;
        }
    }
    free(g->c);
    *g = ng;
}

void vt_resize(struct vt *vt, int rows, int cols)
{
    int shift = 0, y;

    if (rows < 1)
        rows = 1;
    if (cols < 1)
        cols = 1;
    if (rows == vt->rows && cols == vt->cols)
        return;
    /* Getting shorter: lines above the cursor go to the history so the
     * cursor's line stays on screen. */
    if (vt->y > rows - 1)
        shift = vt->y - (rows - 1);
    if (vt->g == &vt->main)
        for (y = 0; y < shift; y++)
            sb_push(&vt->sb, &vt->main.c[y * vt->cols], vt->cols);
    resize_grid(vt, &vt->main, rows, cols, vt->g == &vt->main ? shift : 0);
    if (vt->alt.c != NULL)
        resize_grid(vt, &vt->alt, rows, cols, vt->g == &vt->alt ? shift : 0);
    vt->rows = rows;
    vt->cols = cols;
    vt->y -= shift;
    if (vt->x >= cols)
        vt->x = cols - 1;
    vt->wrapnext = 0;
    vt->top = 0;
    vt->bot = rows - 1;
    free(vt->tabs);
    vt->tabs = xmalloc((size_t)cols);
    reset_tabs(vt);
    free(vt->rowdirty);
    vt->rowdirty = xmalloc((size_t)rows);
    touch(vt, 0, rows - 1);
    vt->dirty = 1;
}
