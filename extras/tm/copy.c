/*
 * copy.c - copy mode. See copy.h.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "copy.h"
#include "keys.h"
#include "util.h"

#define A_MODE MKATTR(0, 3, 0)      /* black on yellow, tmux's mode-style */

/* History lines, or 0 while the alternate screen is up (full-screen
 * programs have no history of their own). */
static long history(const struct window *w)
{
    return w->vt.g == &w->vt.main ? sb_count(&w->vt.sb) : 0;
}

static long total(const struct window *w)
{
    return history(w) + w->vt.rows;
}

/* Line i of the combined list, as `cols` cells. */
static void get_line(struct window *w, long i, struct cell *out, int cols)
{
    long h = history(w);
    int x;

    if (i < h) {
        if (sb_get(&w->vt.sb, i, out, cols))
            return;
    } else if (i - h < w->vt.rows) {
        const struct cell *r = vt_cell(&w->vt, (int)(i - h), 0);

        for (x = 0; x < cols; x++) {
            if (x < w->vt.cols) {
                out[x] = r[x];
            } else {
                out[x].ch = ' ';
                out[x].attr = ATTR_DEFAULT;
            }
        }
        return;
    }
    for (x = 0; x < cols; x++) {
        out[x].ch = ' ';
        out[x].attr = ATTR_DEFAULT;
    }
}

/* Keep positions on the same text when old history is dropped. */
static void adjust(struct window *w)
{
    struct copy *c = &w->cm;
    long d = w->vt.sb.dropped - c->dropped;

    if (d != 0) {
        c->top -= d;
        c->cy -= d;
        c->sy -= d;
        c->dropped = w->vt.sb.dropped;
    }
    if (c->cy < 0)
        c->cy = 0;
    if (c->cy >= total(w))
        c->cy = total(w) - 1;
    if (c->sy < 0)
        c->sy = 0;
}

/* Scroll so the cursor line is visible. */
static void follow(struct window *w, int rows)
{
    struct copy *c = &w->cm;
    long max = total(w) - rows;

    if (c->cy < c->top)
        c->top = c->cy;
    if (c->cy >= c->top + rows)
        c->top = c->cy - rows + 1;
    if (c->top > max)
        c->top = max;
    if (c->top < 0)
        c->top = 0;
}

void copy_enter(struct window *w, int rows)
{
    struct copy *c = &w->cm;

    c->active = 1;
    c->selecting = 0;
    c->dropped = w->vt.sb.dropped;
    c->top = total(w) - rows;
    if (c->top < 0)
        c->top = 0;
    c->cy = history(w) + w->vt.y;
    c->cx = w->vt.x;
    follow(w, rows);
}

/* Index of the last non-blank cell of a line, or -1. */
static int line_end(const struct cell *c, int cols)
{
    int x = cols - 1;

    while (x >= 0 && c[x].ch == ' ' && CELL_BG(c[x].attr) == COL_DEFAULT)
        x--;
    return x;
}

/* Append cells [x0, x1] of a line as UTF-8, without trailing blanks. */
static void add_text(struct buf *b, const struct cell *c, int x0, int x1)
{
    int x, end = x1;
    char u[4];

    while (end >= x0 && c[end].ch == ' ')
        end--;
    for (x = x0; x <= end; x++)
        if (!(c[x].attr & CELL_CONT))
            buf_add(b, u, (size_t)utf8_encode(u, c[x].ch));
}

static void yank_selection(struct window *w, char **yank, size_t *len)
{
    struct copy *c = &w->cm;
    long y0 = c->sy, y1 = c->cy, y;
    int x0 = c->sx, x1 = c->cx, cols = w->vt.cols;
    struct cell *line = xmalloc(sizeof *line * (size_t)cols);
    struct buf b = { NULL, 0, 0 };

    if (y0 > y1 || (y0 == y1 && x0 > x1)) {     /* put them in order */
        long ty = y0;
        int tx = x0;

        y0 = y1;
        x0 = x1;
        y1 = ty;
        x1 = tx;
    }
    for (y = y0; y <= y1; y++) {
        get_line(w, y, line, cols);
        add_text(&b, line, y == y0 ? x0 : 0, y == y1 ? x1 : cols - 1);
        if (y < y1)
            buf_add(&b, "\n", 1);
    }
    free(line);
    *yank = b.s;
    *len = b.len;
}

int copy_key(struct window *w, int key, int rows, char **yank,
             size_t *yank_len)
{
    struct copy *c = &w->cm;
    int cols = w->vt.cols, half = rows / 2 > 0 ? rows / 2 : 1;
    struct cell *line;

    adjust(w);
    switch (key) {
    case K_UP: case 'k':            c->cy--; break;
    case K_DOWN: case 'j':          c->cy++; break;
    case K_LEFT: case 'h':          if (c->cx > 0) c->cx--; break;
    case K_RIGHT: case 'l':         if (c->cx < cols - 1) c->cx++; break;
    case K_PGUP: case K_CTRL('b'):  c->cy -= rows; c->top -= rows; break;
    case K_PGDN: case K_CTRL('f'):  c->cy += rows; c->top += rows; break;
    case K_CTRL('u'):               c->cy -= half; c->top -= half; break;
    case K_CTRL('d'):               c->cy += half; c->top += half; break;
    case 'g': case K_HOME:          c->cy = 0; c->cx = 0; break;
    case 'G': case K_END:           c->cy = total(w) - 1; break;
    case '0':                       c->cx = 0; break;
    case '$':
        line = xmalloc(sizeof *line * (size_t)cols);
        get_line(w, c->cy, line, cols);
        c->cx = line_end(line, cols);
        if (c->cx < 0)
            c->cx = 0;
        free(line);
        break;
    case ' ': case 'v':
        c->selecting = !c->selecting;
        c->sy = c->cy;
        c->sx = c->cx;
        break;
    case K_ENTER:
        if (c->selecting)
            yank_selection(w, yank, yank_len);
        c->active = 0;
        return COPY_EXIT;
    case 'q': case K_ESC: case K_CTRL('c'):
        if (c->selecting && key == K_ESC) {
            c->selecting = 0;           /* first Esc drops the selection */
            break;
        }
        c->active = 0;
        return COPY_EXIT;
    case '/':
        return COPY_SEARCH_DOWN;
    case '?':
        return COPY_SEARCH_UP;
    case 'n': case 'N':
        if (c->search[0] != '\0')
            copy_search(w, c->search, c->search_back ^ (key == 'N'), rows);
        break;
    }
    adjust(w);
    follow(w, rows);
    return COPY_STAY;
}

/* Case-insensitive search for `needle` in the line's text; returns the
 * column or -1. */
static int find_in_line(const struct cell *c, int cols, const char *needle)
{
    size_t n = strlen(needle);
    int x, k;

    for (x = 0; x < cols; x++) {
        int col = x;

        for (k = 0; (size_t)k < n && col < cols; k++, col++) {
            while (col < cols && (c[col].attr & CELL_CONT))
                col++;
            if (col >= cols || c[col].ch > 0x7F ||
                tolower((int)c[col].ch) != tolower((unsigned char)needle[k]))
                break;
        }
        if ((size_t)k == n)
            return x;
    }
    return -1;
}

int copy_search(struct window *w, const char *text, int up, int rows)
{
    struct copy *c = &w->cm;
    int cols = w->vt.cols, found = 0;
    long y, n = total(w);
    struct cell *line;

    if (text[0] == '\0')
        return 0;
    snprintf(c->search, sizeof c->search, "%s", text);
    c->search_back = up;
    line = xmalloc(sizeof *line * (size_t)cols);
    for (y = c->cy + (up ? -1 : 1); y >= 0 && y < n; y += up ? -1 : 1) {
        int x;

        get_line(w, y, line, cols);
        x = find_in_line(line, cols, text);
        if (x >= 0) {
            c->cy = y;
            c->cx = x;
            c->top = y - rows / 2;      /* show it mid-screen */
            found = 1;
            break;
        }
    }
    free(line);
    follow(w, rows);
    return found;
}

int copy_scroll(struct window *w, int lines, int rows)
{
    struct copy *c = &w->cm;
    long max;

    adjust(w);
    max = total(w) - rows;
    if (max < 0)
        max = 0;
    c->top += lines;
    if (c->top > max)
        c->top = max;
    if (c->top < 0)
        c->top = 0;
    if (c->cy < c->top)
        c->cy = c->top;
    if (c->cy >= c->top + rows)
        c->cy = c->top + rows - 1;
    return c->top >= max;
}

void copy_point(struct window *w, int x, int y, int rows, int drag)
{
    struct copy *c = &w->cm;

    adjust(w);
    if (drag && y < 0) {
        copy_scroll(w, -1, rows);
        y = 0;
    } else if (drag && y >= rows) {
        copy_scroll(w, 1, rows);
        y = rows - 1;
    }
    if (y < 0)
        y = 0;
    if (y >= rows)
        y = rows - 1;
    c->cy = c->top + y;
    if (c->cy >= total(w))
        c->cy = total(w) - 1;
    c->cx = x < w->vt.cols ? x : w->vt.cols - 1;
    if (drag) {
        c->selecting = 1;
    } else {
        c->selecting = 0;
        c->sy = c->cy;
        c->sx = c->cx;
    }
}

void copy_finish(struct window *w, char **yank, size_t *yank_len)
{
    if (w->cm.selecting)
        yank_selection(w, yank, yank_len);
    w->cm.active = 0;
}

void copy_draw(struct window *w, struct cell *scr, int rows, int cols,
               int *cx, int *cy)
{
    struct copy *c = &w->cm;
    long y0, y1, y;
    int x0, x1, r, x, len;
    char pos[48];

    adjust(w);
    follow(w, rows);
    y0 = c->sy;
    x0 = c->sx;
    y1 = c->cy;
    x1 = c->cx;
    if (y0 > y1 || (y0 == y1 && x0 > x1)) {
        long ty = y0;
        int tx = x0;

        y0 = y1;
        x0 = x1;
        y1 = ty;
        x1 = tx;
    }
    for (r = 0; r < rows; r++) {
        struct cell *row = scr + r * cols;

        y = c->top + r;
        get_line(w, y, row, cols);
        if (!c->selecting || y < y0 || y > y1)
            continue;
        for (x = 0; x < cols; x++)
            if ((y > y0 || x >= x0) && (y < y1 || x <= x1))
                row[x].attr = A_MODE | (row[x].attr & CELL_CONT);
    }

    /* "[lines scrolled up/history]" at the top right, like tmux */
    len = snprintf(pos, sizeof pos, "[%ld/%ld]",
                   total(w) - rows - c->top, history(w));
    for (x = 0; x < len && x < cols; x++) {
        struct cell *cell = &scr[cols - len + x];

        cell->ch = (unsigned char)pos[x];
        cell->attr = A_MODE;
    }
    *cx = c->cx;
    *cy = (int)(c->cy - c->top);
}
