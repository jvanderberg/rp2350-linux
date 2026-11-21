/*
 * viewer.c - built-in file viewer with a Markdown mode. See viewer.h.
 *
 * The file is mmap()ed, so opening is instant and only the pages that are
 * looked at are read from the SD card. Line starts are indexed lazily, as
 * far as the display needs them; only End and searching backwards from it
 * walk the whole file.
 *
 * Drawing works line by line: render_line() turns one line of the file
 * into a row of cells (character, colour, and the file offset it came
 * from, for search highlighting), possibly with forced breaks in it (a
 * Markdown table's first row also draws the table's top border).
 * layout() then cuts the cells into screen rows: at each forced break,
 * and with wrapping on also every `width` cells. The scroll position is a
 * line number plus a row within that line.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dialog.h"
#include "keys.h"
#include "menu.h"
#include "screen.h"
#include "skin.h"
#include "util.h"
#include "viewer.h"

#define MAX_CELLS 8192      /* longest rendered line; the rest is cut */
#define MAX_BREAKS 8        /* forced row breaks within one line */
#define MAX_ROWS (MAX_CELLS + MAX_BREAKS + 1)
#define MAX_COLS 32         /* table columns */
#define MAX_TABLE 500       /* table rows looked at around a line */

/* Per-line flags from the Markdown pre-scan (fenced code blocks span
 * lines, so they can't be found by looking at one line alone). */
#define L_FENCE 1           /* a ``` or ~~~ line */
#define L_CODE  2           /* inside a fenced block */

struct vcell {
    unsigned ch;
    unsigned short attr;
    long src;               /* file offset of the character, -1 if none */
};

struct vrow {
    int start, end;         /* cells [start, end) form one screen row */
};

/* The layout of the Markdown table around the line being drawn, kept
 * because every row of a table needs the widths of all of them. */
struct table {
    long first, last;       /* its lines (first < 0: none cached) */
    int cols;
    int width[MAX_COLS];    /* widest rendered cell of each column */
    char align[MAX_COLS];   /* 'l', 'c' or 'r' from the delimiter row */
};

struct view {
    const char *path;
    const unsigned char *data;
    long size;

    long *line;             /* start offset of each indexed line */
    unsigned char *lflags;  /* L_* per line */
    long nlines, cap;
    int complete;           /* every line is indexed */
    int in_fence;           /* pre-scan state */

    int wrap, hex, md;
    long top, sub;          /* text mode: first line, row within it */
    long hex_top;           /* hex mode: offset of the first row */
    int hscroll;            /* unwrapped text: first column shown */
    long shown_end;         /* offset just past the last byte on screen */

    long found, found_len;  /* last search match, found < 0 if none */
    char search[128];

    struct vcell *cells;
    int ncells;
    int brk[MAX_BREAKS];    /* forced breaks: cell indices */
    int nbrk;
    struct vrow *rows;      /* layout() result */
    int nrows;
    struct table tbl;
};

static struct view *cur;    /* the open viewer, for the dialog painter */

/* ---- line index ------------------------------------------------------- */

static int is_fence(const unsigned char *p, const unsigned char *end)
{
    int i = 0;

    while (i < 3 && p + i < end && p[i] == ' ')
        i++;
    p += i;
    return end - p >= 3 && ((p[0] == '`' && p[1] == '`' && p[2] == '`') ||
                             (p[0] == '~' && p[1] == '~' && p[2] == '~'));
}

static void add_line(struct view *v, long start)
{
    const unsigned char *p = v->data + start, *end = v->data + v->size;
    const unsigned char *nl = memchr(p, '\n', (size_t)(end - p));

    if (v->nlines == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 1024;
        v->line = realloc(v->line, sizeof *v->line * (size_t)v->cap);
        v->lflags = realloc(v->lflags, (size_t)v->cap);
        if (v->line == NULL || v->lflags == NULL) {
            fputs("mc: out of memory\n", stderr);
            exit(1);
        }
    }
    v->line[v->nlines] = start;
    if (is_fence(p, nl != NULL ? nl : end)) {
        v->lflags[v->nlines] = L_FENCE;
        v->in_fence = !v->in_fence;
    } else {
        v->lflags[v->nlines] = v->in_fence ? L_CODE : 0;
    }
    v->nlines++;
}

/* Index lines until line `want` exists or the file ends. */
static void index_until(struct view *v, long want)
{
    while (!v->complete && v->nlines <= want) {
        long from = v->line[v->nlines - 1];
        const unsigned char *nl = memchr(v->data + from, '\n',
                                         (size_t)(v->size - from));
        long next;

        if (nl == NULL) {
            v->complete = 1;
            break;
        }
        next = (long)(nl - v->data) + 1;
        if (next >= v->size) {      /* a final newline ends the file */
            v->complete = 1;
            break;
        }
        add_line(v, next);
    }
}

/* Byte range of line i, without its newline (and CR). */
static void line_range(struct view *v, long i, long *a, long *b)
{
    const unsigned char *nl;

    *a = v->line[i];
    nl = memchr(v->data + *a, '\n', (size_t)(v->size - *a));
    *b = nl != NULL ? (long)(nl - v->data) : v->size;
    if (*b > *a && v->data[*b - 1] == '\r')
        (*b)--;
}

/* The line containing offset `off`. */
static long line_of(struct view *v, long off)
{
    long lo = 0, hi;

    while (!v->complete && v->line[v->nlines - 1] <= off)
        index_until(v, v->nlines);
    hi = v->nlines - 1;
    while (lo < hi) {
        long mid = (lo + hi + 1) / 2;

        if (v->line[mid] <= off)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

/* ---- rendering a line into cells -------------------------------------- */

static void put(struct view *v, unsigned ch, int attr, long src)
{
    if (v->ncells < MAX_CELLS) {
        struct vcell *c = &v->cells[v->ncells++];

        c->ch = ch;
        c->attr = (unsigned short)attr;
        c->src = src;
    }
}

static void put_str(struct view *v, const char *s, int attr)
{
    while (*s)
        put(v, utf8_next(&s), attr, -1);
}

/* Start a new screen row here, whatever the wrapping. */
static void put_break(struct view *v)
{
    if (v->nbrk < MAX_BREAKS)
        v->brk[v->nbrk++] = v->ncells;
}

/* Decode one character at offset i (bounded by b); sets *len. Bytes that
 * are not valid UTF-8 show as '.', control characters too. */
static unsigned decode(const struct view *v, long i, long b, int *len)
{
    const unsigned char *p = v->data + i;
    unsigned c = p[0];
    int n, k;

    *len = 1;
    if (c < 0x80)
        return c < 0x20 || c == 0x7F ? (c == '\t' ? '\t' : '.') : c;
    n = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2
      : (c & 0xF8) == 0xF0 ? 3 : -1;
    if (n < 0 || i + n >= b)
        return '.';
    c &= 0x3F >> n;
    for (k = 1; k <= n; k++) {
        if ((p[k] & 0xC0) != 0x80)
            return '.';
        c = (c << 6) | (p[k] & 0x3F);
    }
    *len = n + 1;
    return c;
}

/* One character from the file (tabs expand to the next multiple of 8);
 * returns its length in bytes. */
static int put_char(struct view *v, long i, long b, int attr)
{
    int len;
    unsigned c = decode(v, i, b, &len);

    if (c == '\t') {
        do
            put(v, ' ', attr, i);
        while (v->ncells % 8 != 0 && v->ncells < MAX_CELLS);
    } else {
        put(v, c, attr, i);
    }
    return len;
}

static void put_raw(struct view *v, long a, long b, int attr)
{
    while (a < b)
        a += put_char(v, a, b, attr);
}

/* Find the closing `c` (a single char) for emphasis starting at i. */
static long find_close(const struct view *v, long i, long b, unsigned char c,
                       int twice)
{
    const unsigned char *d = v->data;

    for (i++; i < b; i++) {
        if (d[i] == '\\') {
            i++;
            continue;
        }
        if (d[i] == c && !isspace(d[i - 1]) &&
            (!twice || (i + 1 < b && d[i + 1] == c)))
            return i;
    }
    return -1;
}

/* Inline Markdown: `code`, **bold**, *italic*, [links](url), ![images],
 * <autolinks> and backslash escapes, in base colour `base`. */
static void put_inline(struct view *v, long a, long b, int base)
{
    const unsigned char *d = v->data;
    int bold = 0, ital = 0;
    long i = a, j, k;

    while (i < b) {
        unsigned char c = d[i];
        int attr = bold && base == A_VIEW ? A_MD_BOLD
                 : ital && base == A_VIEW ? A_MD_ITALIC : base;

        if (c == '\\' && i + 1 < b && ispunct(d[i + 1])) {
            i += 1 + put_char(v, i + 1, b, attr);
            continue;
        }
        if (c == '`') {                 /* code span: `x` or ``x`` */
            int n = 0;

            while (i + n < b && d[i + n] == '`')
                n++;
            for (j = i + n; j + n <= b; j++) {
                for (k = 0; k < n && d[j + k] == '`'; k++)
                    ;
                if (k == n && (j + n == b || d[j + n] != '`'))
                    break;
            }
            if (j + n <= b) {
                put_raw(v, i + n, j, A_MD_CODE);
                i = j + n;
                continue;
            }
        }
        if ((c == '*' || c == '_') && i + 1 < b && d[i + 1] == c) {
            if (bold || find_close(v, i + 1, b, c, 1) > 0) {
                bold = !bold;
                i += 2;
                continue;
            }
        }
        if (c == '*' || c == '_') {
            int word_inside = c == '_' && i > a && isalnum(d[i - 1]) &&
                              i + 1 < b && isalnum(d[i + 1]);

            if (!word_inside) {
                if (ital && i > a && !isspace(d[i - 1])) {
                    ital = 0;
                    i++;
                    continue;
                }
                if (!ital && i + 1 < b && !isspace(d[i + 1]) &&
                    find_close(v, i, b, c, 0) > 0) {
                    ital = 1;
                    i++;
                    continue;
                }
            }
        }
        if (c == '[' || (c == '!' && i + 1 < b && d[i + 1] == '[')) {
            long t0 = i + (c == '!' ? 2 : 1), t1, u0, u1;

            t1 = t0;
            while (t1 < b && d[t1] != ']')
                t1++;
            if (t1 + 1 < b && d[t1 + 1] == '(') {
                u0 = t1 + 2;
                u1 = u0;
                while (u1 < b && d[u1] != ')')
                    u1++;
                if (u1 < b) {
                    if (c == '!') {
                        put_str(v, "[image: ", A_MD_RULE);
                        put_raw(v, t0, t1, A_MD_RULE);
                        put_str(v, "]", A_MD_RULE);
                    } else {
                        put_raw(v, t0, t1, A_MD_LINK);
                        if (d[u0] != '#') {     /* skip in-page anchors */
                            put_str(v, " <", A_MD_RULE);
                            put_raw(v, u0, u1, A_MD_RULE);
                            put_str(v, ">", A_MD_RULE);
                        }
                    }
                    i = u1 + 1;
                    continue;
                }
            }
        }
        if (c == '<' && b - i > 8 &&
            (memcmp(d + i + 1, "http", 4) == 0 ||
             memcmp(d + i + 1, "mailto", 6) == 0)) {
            j = i + 1;
            while (j < b && d[j] != '>' && d[j] != ' ')
                j++;
            if (j < b && d[j] == '>') {
                put_raw(v, i + 1, j, A_MD_LINK);
                i = j + 1;
                continue;
            }
        }
        i += put_char(v, i, b, attr);
    }
}

static int is_blank(const struct view *v, long a, long b)
{
    while (a < b && (v->data[a] == ' ' || v->data[a] == '\t'))
        a++;
    return a == b;
}

/* A table delimiter row: only | - : and spaces, with at least one '-'. */
static int is_table_rule(const struct view *v, long a, long b)
{
    int dash = 0;

    for (; a < b; a++) {
        unsigned char c = v->data[a];

        if (c == '-')
            dash = 1;
        else if (c != '|' && c != ':' && c != ' ')
            return 0;
    }
    return dash;
}

/* Number of leading spaces (a tab counts as 4). */
static int indent_of(const struct view *v, long a, long b, long *p)
{
    int n = 0;

    while (a < b && (v->data[a] == ' ' || v->data[a] == '\t')) {
        n += v->data[a] == '\t' ? 4 : 1;
        a++;
    }
    *p = a;
    return n;
}

/* ---- Markdown tables ------------------------------------------------- */

/* Could line ln be part of a table: plain text containing a '|'? */
static int has_pipe(struct view *v, long ln)
{
    long a, b;

    if (v->lflags[ln] & (L_CODE | L_FENCE))
        return 0;
    line_range(v, ln, &a, &b);
    return !is_blank(v, a, b) &&
           memchr(v->data + a, '|', (size_t)(b - a)) != NULL;
}

/* Split a table row into trimmed cells [ca[i], cb[i]). Outer pipes are
 * optional; "\|" and pipes inside `code` don't split. */
static int split_row(const struct view *v, long a, long b, long *ca, long *cb)
{
    const unsigned char *d = v->data;
    long p, start;
    int n = 0, code = 0;

    while (a < b && (d[a] == ' ' || d[a] == '\t'))
        a++;
    while (b > a && (d[b - 1] == ' ' || d[b - 1] == '\t'))
        b--;
    if (a < b && d[a] == '|')
        a++;
    if (b > a && d[b - 1] == '|' && !(b - 2 >= a && d[b - 2] == '\\'))
        b--;
    for (start = p = a; p <= b; p++) {
        if (p < b && d[p] == '`')
            code = !code;
        if (p == b || (d[p] == '|' && !code && (p == a || d[p - 1] != '\\'))) {
            long s0 = start, e0 = p;

            while (s0 < e0 && d[s0] == ' ')
                s0++;
            while (e0 > s0 && d[e0 - 1] == ' ')
                e0--;
            if (n < MAX_COLS) {
                ca[n] = s0;
                cb[n] = e0;
                n++;
            }
            start = p + 1;
        }
    }
    return n;
}

/* Width of [a, b) once rendered (Markdown markup takes no room). */
static int inline_width(struct view *v, long a, long b)
{
    int save = v->ncells, w;

    put_inline(v, a, b, A_VIEW);
    w = v->ncells - save;
    v->ncells = save;
    return w;
}

/* Is line ln in a table (a header row, then a delimiter row like
 * "|---|:--:|", then body rows)? Measures it into v->tbl if so. */
static int table_at(struct view *v, long ln)
{
    struct table *t = &v->tbl;
    long first = ln, last = ln, i, a, b, ca[MAX_COLS], cb[MAX_COLS];
    int n, c;

    if (t->first >= 0 && ln >= t->first && ln <= t->last)
        return 1;
    if (!has_pipe(v, ln))
        return 0;
    while (first > 0 && ln - first < MAX_TABLE && has_pipe(v, first - 1))
        first--;
    for (;;) {
        index_until(v, last + 1);
        if (last + 1 >= v->nlines || last - ln >= MAX_TABLE ||
            !has_pipe(v, last + 1))
            break;
        last++;
    }
    if (last == first)
        return 0;
    line_range(v, first + 1, &a, &b);
    if (!is_table_rule(v, a, b))
        return 0;

    t->first = first;
    t->last = last;
    t->cols = split_row(v, a, b, ca, cb);
    for (c = 0; c < t->cols; c++) {
        int left = ca[c] < cb[c] && v->data[ca[c]] == ':';
        int right = ca[c] < cb[c] && v->data[cb[c] - 1] == ':';

        t->align[c] = left && right ? 'c' : right ? 'r' : 'l';
        t->width[c] = 3;
    }
    for (i = first; i <= last; i++) {
        if (i == first + 1)
            continue;
        line_range(v, i, &a, &b);
        n = split_row(v, a, b, ca, cb);
        for (c = 0; c < n && c < t->cols; c++) {
            int w = inline_width(v, ca[c], cb[c]);

            if (w > t->width[c])
                t->width[c] = w;
        }
    }
    return 1;
}

/* A border line: left, the column runs joined by mid, then right. */
static void put_border(struct view *v, unsigned left, unsigned mid,
                       unsigned right)
{
    const struct table *t = &v->tbl;
    int c, k;

    put(v, left, A_MD_RULE, -1);
    for (c = 0; c < t->cols; c++) {
        for (k = 0; k < t->width[c] + 2; k++)
            put(v, BOX_H, A_MD_RULE, -1);
        put(v, c == t->cols - 1 ? right : mid, A_MD_RULE, -1);
    }
}

static void render_table_row(struct view *v, long ln, long a, long b)
{
    const struct table *t = &v->tbl;
    long ca[MAX_COLS], cb[MAX_COLS];
    int n, c;

    if (ln == t->first) {
        put_border(v, BOX_TL, 0x252C, BOX_TR);          /* ┌─┬─┐ */
        put_break(v);
    }
    if (ln == t->first + 1) {
        put_border(v, BOX_LT, 0x253C, BOX_RT);          /* ├─┼─┤ */
    } else {
        n = split_row(v, a, b, ca, cb);
        put(v, BOX_V, A_MD_RULE, -1);
        for (c = 0; c < t->cols; c++) {
            int start, len, pad, before;

            put(v, ' ', A_VIEW, -1);
            start = v->ncells;
            if (c < n)
                put_inline(v, ca[c], cb[c],
                           ln == t->first ? A_MD_BOLD : A_VIEW);
            len = v->ncells - start;
            pad = t->width[c] > len ? t->width[c] - len : 0;
            before = t->align[c] == 'r' ? pad : t->align[c] == 'c' ? pad / 2
                                                                   : 0;
            if (before > 0 && v->ncells + before <= MAX_CELLS) {
                int k;

                memmove(v->cells + start + before, v->cells + start,
                        sizeof *v->cells * (size_t)len);
                for (k = 0; k < before; k++) {
                    v->cells[start + k].ch = ' ';
                    v->cells[start + k].attr = A_VIEW;
                    v->cells[start + k].src = -1;
                }
                v->ncells += before;
            }
            while (v->ncells < start + len + pad)
                put(v, ' ', A_VIEW, -1);
            put(v, ' ', A_VIEW, -1);
            put(v, BOX_V, A_MD_RULE, -1);
        }
    }
    if (ln == t->last) {
        put_break(v);
        put_border(v, BOX_BL, 0x2534, BOX_BR);          /* └─┴─┘ */
    }
}

static void render_markdown(struct view *v, long ln, long a, long b, int width)
{
    const unsigned char *d = v->data;
    long p, q;
    int ind;

    if (v->lflags[ln] & L_FENCE) {
        put_raw(v, a, b, A_MD_RULE);
        return;
    }
    if (v->lflags[ln] & L_CODE) {
        put_raw(v, a, b, A_MD_CODE);
        return;
    }
    ind = indent_of(v, a, b, &p);
    if (p == b)
        return;

    /* "    code": indented code after a blank line (or more code) */
    if (ind >= 4 && ln > 0 && !(d[p] == '-' || d[p] == '*' || d[p] == '+' ||
                                isdigit(d[p]))) {
        long pa, pb, pp;

        line_range(v, ln - 1, &pa, &pb);
        if (is_blank(v, pa, pb) || indent_of(v, pa, pb, &pp) >= 4) {
            put_raw(v, a, b, A_MD_CODE);
            return;
        }
    }

    if (table_at(v, ln)) {
        render_table_row(v, ln, a, b);
        return;
    }

    /* # Heading */
    if (d[p] == '#') {
        int level = 0;

        while (p + level < b && d[p + level] == '#')
            level++;
        if (level <= 6 && (p + level == b || d[p + level] == ' ')) {
            q = b;
            while (q > p + level && (d[q - 1] == '#' || d[q - 1] == ' '))
                q--;            /* closing #'s */
            p += level;
            while (p < q && d[p] == ' ')
                p++;
            if (level == 1)                 /* a full-width band */
                put(v, ' ', A_MD_H1, -1);
            put_inline(v, p, q, level == 1 ? A_MD_H1
                               : level == 2 ? A_MD_H2 : A_MD_H3);
            while (level == 1 && v->ncells % width != 0)
                put(v, ' ', A_MD_H1, -1);
            return;
        }
    }

    /* --- *** ___ horizontal rule */
    if (d[p] == '-' || d[p] == '*' || d[p] == '_') {
        unsigned char c = d[p];
        int count = 0;

        for (q = p; q < b && (d[q] == c || d[q] == ' '); q++)
            count += d[q] == c;
        if (q == b && count >= 3) {
            while (v->ncells < width)
                put(v, BOX_H, A_MD_RULE, a);
            return;
        }
    }

    /* > quote */
    if (d[p] == '>') {
        while (p < b && d[p] == '>') {
            put_str(v, "\xe2\x94\x82 ", A_MD_RULE);     /* "│ " */
            p++;
            if (p < b && d[p] == ' ')
                p++;
        }
        put_inline(v, p, b, A_MD_QUOTE);
        return;
    }

    /* - bullet, * bullet, + bullet, 1. numbered */
    if ((d[p] == '-' || d[p] == '*' || d[p] == '+') && p + 1 < b &&
        d[p + 1] == ' ') {
        while (v->ncells < ind)
            put(v, ' ', A_VIEW, -1);
        put(v, 0x2022, A_MD_MARK, p);                    /* • */
        put(v, ' ', A_VIEW, -1);
        p += 2;
        if (b - p >= 4 && d[p] == '[' && d[p + 2] == ']' && d[p + 3] == ' ') {
            put_raw(v, p, p + 4, A_MD_MARK);            /* [ ] / [x] */
            p += 4;
        }
        put_inline(v, p, b, A_VIEW);
        return;
    }
    for (q = p; q < b && isdigit(d[q]); q++)
        ;
    if (q > p && q - p < 10 && q + 1 < b && (d[q] == '.' || d[q] == ')') &&
        d[q + 1] == ' ') {
        while (v->ncells < ind)
            put(v, ' ', A_VIEW, -1);
        put_raw(v, p, q + 1, A_MD_MARK);
        put(v, ' ', A_VIEW, -1);
        put_inline(v, q + 2, b, A_VIEW);
        return;
    }

    /* a paragraph line */
    while (v->ncells < ind)
        put(v, ' ', A_VIEW, -1);
    put_inline(v, p, b, A_VIEW);
}

/* Render line ln into v->cells; returns the number of cells. */
static int render_line(struct view *v, long ln)
{
    long a, b;

    v->ncells = 0;
    v->nbrk = 0;
    line_range(v, ln, &a, &b);
    if (v->md)
        render_markdown(v, ln, a, b, scr_cols);
    else
        put_raw(v, a, b, A_VIEW);
    return v->ncells;
}

/* Cut the rendered cells into screen rows (v->rows); returns the count.
 * Every forced break starts a row; with wrapping, so does every
 * scr_cols-th cell of a segment. */
static int layout(struct view *v)
{
    int seg, start = 0;

    v->nrows = 0;
    for (seg = 0; seg <= v->nbrk; seg++) {
        int end = seg < v->nbrk ? v->brk[seg] : v->ncells, s = start;

        do {
            int e = v->wrap && end - s > scr_cols ? s + scr_cols : end;

            if (v->nrows < MAX_ROWS) {
                v->rows[v->nrows].start = s;
                v->rows[v->nrows].end = e;
                v->nrows++;
            }
            s = e;
        } while (s < end);
        start = end;
    }
    return v->nrows;
}

/* Screen rows that line ln takes. */
static long line_rows(struct view *v, long ln)
{
    render_line(v, ln);
    return layout(v);
}

/* ---- scrolling ------------------------------------------------------- */

static int body_rows(void)
{
    return scr_rows - 2 > 1 ? scr_rows - 2 : 1;
}

static int hex_width(void)
{
    return scr_cols >= 78 ? 16 : 8;
}

static long hex_max_top(const struct view *v)
{
    long bpr = hex_width(), rows = (v->size + bpr - 1) / bpr;

    return rows > body_rows() ? (rows - body_rows()) * bpr : 0;
}

/* Is the end of the file already on screen (so Down should stop)? */
static int end_visible(struct view *v)
{
    long ln = v->top, rows = body_rows() + v->sub;

    if (v->hex)
        return v->hex_top >= hex_max_top(v);
    for (;;) {
        rows -= line_rows(v, ln);
        if (rows < 0)
            return 0;
        index_until(v, ln + 1);
        if (++ln >= v->nlines)
            return 1;
    }
}

static void scroll_down(struct view *v, long n)
{
    while (n-- > 0 && !end_visible(v)) {
        if (v->hex) {
            v->hex_top += hex_width();
        } else if (v->sub + 1 < line_rows(v, v->top)) {
            v->sub++;
        } else {
            v->top++;
            v->sub = 0;
        }
    }
}

static void scroll_up(struct view *v, long n)
{
    while (n-- > 0) {
        if (v->hex) {
            if (v->hex_top >= hex_width())
                v->hex_top -= hex_width();
        } else if (v->sub > 0) {
            v->sub--;
        } else if (v->top > 0) {
            v->top--;
            v->sub = line_rows(v, v->top) - 1;
        }
    }
}

static void go_end(struct view *v)
{
    if (v->hex) {
        v->hex_top = hex_max_top(v);
        return;
    }
    index_until(v, 0x7FFFFFFFL);
    v->top = v->nlines - 1;
    v->sub = line_rows(v, v->top) - 1;
    scroll_up(v, body_rows() - 1);
}

/* Put the line (or hex row) holding offset `off` at the top. */
static void go_offset(struct view *v, long off)
{
    if (v->hex) {
        v->hex_top = off - off % hex_width();
        if (v->hex_top > hex_max_top(v))
            v->hex_top = hex_max_top(v);
        return;
    }
    v->top = line_of(v, off);
    v->sub = 0;
    {                   /* the row of the line that shows the offset */
        int i, r, n = render_line(v, v->top);

        layout(v);
        for (i = 0; i < n; i++)
            if (v->cells[i].src >= off)
                break;
        for (r = 0; r < v->nrows && i < n; r++)
            if (i >= v->rows[r].start && i < v->rows[r].end) {
                v->sub = r;
                break;
            }
    }
}

/* ---- drawing ---------------------------------------------------------- */

static int cell_attr(const struct view *v, const struct vcell *c)
{
    if (v->found >= 0 && c->src >= v->found &&
        c->src < v->found + v->found_len)
        return A_VIEW_FOUND;
    return c->attr;
}

static void draw_text(struct view *v)
{
    int y = 1, last = body_rows(), x;
    long ln = v->top, r = v->sub;

    v->shown_end = v->size;
    while (y <= last) {
        long rows, a, b;
        int n;

        index_until(v, ln);
        if (ln >= v->nlines)
            break;
        n = render_line(v, ln);
        rows = layout(v);
        for (; r < rows && y <= last; r++, y++) {
            const struct vrow *row = &v->rows[r];
            int base = row->start + (v->wrap ? 0 : v->hscroll);

            for (x = 0; x < scr_cols && base + x < row->end && base + x < n;
                 x++) {
                const struct vcell *c = &v->cells[base + x];

                scr_put(y, x, c->ch, cell_attr(v, c));
            }
        }
        line_range(v, ln, &a, &b);
        v->shown_end = b < v->size ? b + 1 : b;
        ln++;
        r = 0;
    }
}

static void draw_hex(struct view *v)
{
    int bpr = hex_width(), y, j;
    char buf[16];

    for (y = 1; y <= body_rows(); y++) {
        long off = v->hex_top + (long)(y - 1) * bpr;
        int x = 9;

        if (off >= v->size)
            break;
        sprintf(buf, "%08lX", (unsigned long)off);
        scr_puts(y, 0, buf, A_VIEW_DIM);
        for (j = 0; j < bpr && off + j < v->size; j++) {
            unsigned char c = v->data[off + j];
            struct vcell cell;
            int attr;

            cell.src = off + j;
            cell.attr = A_VIEW;
            attr = cell_attr(v, &cell);
            sprintf(buf, "%02X", c);
            scr_puts(y, x, buf, attr);
            x += 3 + ((j + 1) % 4 == 0);
            scr_put(y, 10 + bpr * 3 + bpr / 4 + j,
                    c >= 0x20 && c < 0x7F ? c : '.', attr);
        }
        v->shown_end = off + j;
    }
}

static void view_paint(void)
{
    struct view *v = cur;
    char buf[64];
    int pct;
    const char *keys[10];

    scr_fill(0, 0, scr_rows, scr_cols, ' ', A_VIEW);
    if (v->hex)
        draw_hex(v);
    else
        draw_text(v);

    /* status line: name, position/size, percentage */
    scr_fill(0, 0, 1, scr_cols, ' ', A_VIEW_STATUS);
    scr_text(0, 0, scr_cols > 40 ? scr_cols - 34 : scr_cols / 2, v->path,
             A_VIEW_STATUS, T_LEFT | T_TRIM_MID);
    if (scr_cols > 40) {
        pct = v->size > 0 ? (int)((double)v->shown_end * 100 / v->size) : 100;
        sprintf(buf, "%ld/%ld", v->shown_end, v->size);
        scr_puts(0, scr_cols - 26, buf, A_VIEW_STATUS);
        sprintf(buf, "%3d%%", pct);
        scr_puts(0, scr_cols - 4, buf, A_VIEW_STATUS);
    }

    keys[0] = "Help";
    keys[1] = v->wrap ? "UnWrap" : "Wrap";
    keys[2] = "Quit";
    keys[3] = v->hex ? "Ascii" : "Hex";
    keys[4] = "Goto";
    keys[5] = "";
    keys[6] = "Search";
    keys[7] = "";
    keys[8] = v->md ? "Unform" : "Format";
    keys[9] = "Quit";
    keybar_draw(scr_rows - 1, keys);
}

/* ---- commands --------------------------------------------------------- */

static void search(struct view *v, int ask)
{
    long start, i, n;

    if (ask || v->search[0] == '\0') {
        if (!dlg_input_box("Search", "Enter search string:", v->search,
                           sizeof v->search, 0) || v->search[0] == '\0')
            return;
        v->found = -1;
    }
    n = (long)strlen(v->search);
    if (v->found >= 0)
        start = v->found + 1;
    else
        start = v->hex ? v->hex_top : v->line[v->top];
    for (i = start; i + n <= v->size; i++)
        if (strncasecmp((const char *)v->data + i, v->search, (size_t)n) == 0)
            break;
    if (i + n > v->size) {
        dlg_message("Search", 0, "Search string not found");
        v->found = -1;
        return;
    }
    v->found = i;
    v->found_len = n;
    go_offset(v, i);
}

static void go_to(struct view *v)
{
    static char buf[32];
    long n;

    if (!dlg_input_box("Goto", v->hex ? "Offset (0x for hex):" : "Line number:",
                       buf, sizeof buf, 0))
        return;
    n = strtol(buf, NULL, 0);
    if (v->hex) {
        go_offset(v, n < 0 ? 0 : n >= v->size ? v->size - 1 : n);
    } else {
        index_until(v, n - 1);
        v->top = n < 1 ? 0 : n - 1 >= v->nlines ? v->nlines - 1 : n - 1;
        v->sub = 0;
    }
}

static void show_help(void)
{
    dlg_message("Viewer", 0,
                "Up/Down PgUp/PgDn Space Home/End  scroll\n"
                "Left/Right       sideways (unwrapped)\n"
                "F2 wrap   F4 hex   F5 goto   F9 markdown\n"
                "F7 or /  search    n  next match\n"
                "F3 F10 q Esc  quit");
}

/* Toggle hex mode, keeping roughly the same place in the file. */
static void toggle_hex(struct view *v)
{
    long off = v->hex ? v->hex_top : v->line[v->top];

    v->hex = !v->hex;
    go_offset(v, off);
}

static int ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);

    return n >= m && strcasecmp(s + n - m, suffix) == 0;
}

int view_file(const char *path)
{
    struct view v;
    struct stat st;
    dlg_painter old;
    int fd, done = 0;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    memset(&v, 0, sizeof v);
    v.path = path;
    v.size = (long)st.st_size;
    if (v.size > 0) {
        void *m = mmap(NULL, (size_t)v.size, PROT_READ, MAP_PRIVATE, fd, 0);

        if (m == MAP_FAILED) {
            int err = errno;

            close(fd);
            errno = err;
            return -1;
        }
        v.data = m;
    } else {
        v.data = (const unsigned char *)"";
    }
    close(fd);

    v.wrap = 1;
    v.md = ends_with(path, ".md") || ends_with(path, ".markdown");
    v.found = -1;
    v.cells = xmalloc(sizeof *v.cells * MAX_CELLS);
    v.rows = xmalloc(sizeof *v.rows * MAX_ROWS);
    v.tbl.first = -1;
    add_line(&v, 0);

    cur = &v;
    old = dlg_set_painter(view_paint);
    while (!done) {
        int k, page = body_rows() - 1 > 0 ? body_rows() - 1 : 1;

        view_paint();
        scr_cursor(-1, 0);
        scr_flush();
        k = key_get(-1);
        if (k == K_MOUSE) {
            if (key_mouse.release)
                continue;
            if (key_mouse.button == 64 || key_mouse.button == 65) {
                if (key_mouse.button == 64)
                    scroll_up(&v, 3);
                else
                    scroll_down(&v, 3);
                continue;
            }
            if (key_mouse.y != scr_rows - 1 || key_mouse.button != 0)
                continue;
            k = K_F(keybar_hit(key_mouse.x));
        }
        switch (k) {
        case K_RESIZE:
            scr_update_size();
            v.sub = 0;
            break;
        case K_QUIT:
        case K_ESC:
        case 'q':
        case K_F(3):
        case K_F(10):
            done = 1;
            break;
        case K_UP:
        case 'k':
            scroll_up(&v, 1);
            break;
        case K_DOWN:
        case 'j':
        case K_ENTER:
            scroll_down(&v, 1);
            break;
        case K_PGUP:
        case 'b':
            scroll_up(&v, page);
            break;
        case K_PGDN:
        case ' ':
            scroll_down(&v, page);
            break;
        case K_HOME:
        case 'g':
            v.top = v.sub = v.hex_top = 0;
            break;
        case K_END:
        case 'G':
            go_end(&v);
            break;
        case K_LEFT:
            if (!v.wrap && v.hscroll > 0)
                v.hscroll -= v.hscroll >= 8 ? 8 : v.hscroll;
            break;
        case K_RIGHT:
            if (!v.wrap)
                v.hscroll += 8;
            break;
        case K_F(1):
            show_help();
            break;
        case K_F(2):
            v.wrap = !v.wrap;
            v.sub = 0;
            v.hscroll = 0;
            break;
        case K_F(4):
            toggle_hex(&v);
            break;
        case K_F(5):
            go_to(&v);
            break;
        case K_F(7):
        case '/':
            search(&v, 1);
            break;
        case 'n':
            search(&v, 0);
            break;
        case K_F(9):
            v.md = !v.md;
            v.sub = 0;
            break;
        }
    }
    dlg_set_painter(old);
    cur = NULL;

    if (v.size > 0)
        munmap((void *)v.data, (size_t)v.size);
    free(v.line);
    free(v.lflags);
    free(v.cells);
    free(v.rows);
    return 0;
}
