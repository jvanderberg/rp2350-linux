/*
 * render.c - diff-based terminal output. See render.h.
 */
#include <stdlib.h>
#include <string.h>

#include "render.h"
#include "vt.h"

void term_reset(struct term *t, int rows, int cols)
{
    size_t i, n = (size_t)rows * (size_t)cols;

    free(t->front);
    t->rows = rows;
    t->cols = cols;
    t->front = xmalloc(sizeof *t->front * n);
    for (i = 0; i < n; i++) {
        t->front[i].ch = 0;             /* matches nothing real */
        t->front[i].attr = 0xFFFFFFFFu;
    }
    t->cursor_on = -1;
}

void term_free(struct term *t)
{
    free(t->front);
    t->front = NULL;
}

static void put_color(struct buf *out, uint32_t c, int base)
{
    if (c == COL_DEFAULT)
        return;                         /* "0" already reset it */
    if (c < 8)
        buf_printf(out, ";%d", base + (int)c);
    else if (c < 16)
        buf_printf(out, ";%d", base + 60 + (int)c - 8);
    else
        buf_printf(out, ";%d;5;%d", base + 8, (int)c);
}

static void put_attr(struct buf *out, uint32_t a)
{
    uint32_t fl = ATTR_FL(a);

    buf_str(out, "\x1b[0");
    if (fl & AT_BOLD)    buf_str(out, ";1");
    if (fl & AT_DIM)     buf_str(out, ";2");
    if (fl & AT_ITALIC)  buf_str(out, ";3");
    if (fl & AT_UNDER)   buf_str(out, ";4");
    if (fl & AT_BLINK)   buf_str(out, ";5");
    if (fl & AT_REVERSE) buf_str(out, ";7");
    if (fl & AT_HIDDEN)  buf_str(out, ";8");
    if (fl & AT_STRIKE)  buf_str(out, ";9");
    put_color(out, ATTR_FG(a), 30);
    put_color(out, ATTR_BG(a), 40);
    buf_str(out, "m");
}

static int same(const struct cell *a, const struct cell *b)
{
    return a->ch == b->ch && a->attr == b->attr;
}

void render_frame(struct buf *out, struct term *t, const struct cell *scr,
                  const unsigned char *rows, int cx, int cy)
{
    uint32_t attr = 0xFFFFFFFFu;
    int y, x, tx = -1, ty = -1, drew = 0;

    for (y = 0; y < t->rows; y++) {
        if (rows != NULL && !rows[y])
            continue;
        for (x = 0; x < t->cols; x++) {
            int i = y * t->cols + x, w = 1;
            const struct cell *c = &scr[i];
            char u[4];

            if (c->attr & CELL_CONT)
                continue;               /* drawn with its left half */
            if (x + 1 < t->cols && (scr[i + 1].attr & CELL_CONT))
                w = 2;
            if (same(c, &t->front[i]) &&
                (w == 1 || same(&scr[i + 1], &t->front[i + 1])))
                continue;
            if (!drew) {
                buf_str(out, "\x1b[?25l");
                t->cursor_on = 0;
                drew = 1;
            }
            if (ty != y || tx != x)
                buf_printf(out, "\x1b[%d;%dH", y + 1, x + 1);
            if (c->attr != attr) {
                attr = c->attr;
                put_attr(out, attr);
            }
            buf_add(out, u, (size_t)utf8_encode(u, c->ch < 0x20 ? ' ' : c->ch));
            t->front[i] = *c;
            if (w == 2)
                t->front[i + 1] = scr[i + 1];
            tx = x + w;
            ty = y;
        }
    }
    if (drew || t->cursor_on != (cy >= 0)) {
        if (cy >= 0) {
            buf_printf(out, "\x1b[%d;%dH\x1b[?25h", cy + 1, cx + 1);
            t->cursor_on = 1;
        } else {
            buf_str(out, "\x1b[?25l");
            t->cursor_on = 0;
        }
    } else if (cy >= 0) {
        buf_printf(out, "\x1b[%d;%dH", cy + 1, cx + 1);
    }
}

void render_modes(struct buf *out, struct term *t, int app_cursor,
                  int app_keypad, int mouse, int paste)
{
    static const struct { int bit, code; } mm[] = {
        { MOUSE_CLICK, 1000 }, { MOUSE_DRAG, 1002 }, { MOUSE_ANY, 1003 },
        { MOUSE_SGR, 1006 }
    };
    int i;

    if (app_cursor != t->app_cursor)
        buf_str(out, app_cursor ? "\x1b[?1h" : "\x1b[?1l");
    if (app_keypad != t->app_keypad)
        buf_str(out, app_keypad ? "\x1b=" : "\x1b>");
    /* Terminals keep 1000/1002/1003 as one setting, and turning any of
     * them off turns mouse reporting off. So on any change: everything
     * off, then the wanted modes on, in order. */
    if (mouse != t->mouse) {
        buf_str(out, "\x1b[?1006l\x1b[?1003l\x1b[?1002l\x1b[?1000l");
        for (i = 0; i < 4; i++)
            if (mouse & mm[i].bit)
                buf_printf(out, "\x1b[?%dh", mm[i].code);
    }
    if (paste != t->paste)
        buf_str(out, paste ? "\x1b[?2004h" : "\x1b[?2004l");
    t->app_cursor = app_cursor;
    t->app_keypad = app_keypad;
    t->mouse = mouse;
    t->paste = paste;
}
