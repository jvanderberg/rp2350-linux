/*
 * vt.h - a terminal emulator: turns a program's output into a grid of
 * cells.
 *
 * It implements what the `screen-256color` terminfo entry advertises
 * (which is what panes run with), plus the common xterm extras programs
 * use anyway:
 *
 *   cursor movement, absolute and relative; save/restore (ESC 7/8, CSI s/u)
 *   erase in line/display, insert/delete characters and lines, ECH, REP
 *   scroll regions (DECSTBM), index/reverse index, SU/SD
 *   tab stops (HTS, TBC, CHT, CBT)
 *   SGR: bold, dim, italic, underline, blink, reverse, hidden, strike;
 *        8/16/256 colours, and truecolour mapped to the 256 palette
 *   modes: insert (IRM), autowrap, origin, application cursor keys and
 *        keypad, cursor visibility, alternate screen (47/1047/1049),
 *        mouse reporting (1000/1002/1003/1006), bracketed paste (2004)
 *   DEC special graphics charset via G0/G1 and SO/SI (line drawing)
 *   UTF-8, double-width characters
 *   replies to device attribute and status queries (DA, DSR/CPR)
 *
 * Lines scrolled off the top of the main screen go to the scrollback.
 */
#ifndef VT_H
#define VT_H

#include <stddef.h>

#include "cell.h"
#include "sb.h"

/* vt.mouse bits */
#define MOUSE_CLICK  1      /* 1000 */
#define MOUSE_DRAG   2      /* 1002 */
#define MOUSE_ANY    4      /* 1003 */
#define MOUSE_SGR    8      /* 1006 */

struct grid {
    struct cell *c;         /* rows * cols cells */
};

struct cursor_save {
    int x, y;
    uint32_t attr;
    int origin, gl;
    char g[2];
};

struct vt {
    int rows, cols;
    struct grid main, alt;
    struct grid *g;         /* the screen being shown: &main or &alt */
    int x, y;               /* cursor */
    int wrapnext;           /* at the right margin, next char wraps */
    uint32_t attr;          /* current pen */
    int top, bot;           /* scroll region, inclusive */
    unsigned char *tabs;    /* 1 where a tab stop is set */

    /* modes */
    int autowrap, origin, insert, cursor_visible;
    int app_cursor, app_keypad, mouse, bracketed_paste;

    /* character sets: g[0]/g[1] are 'B' (ASCII) or '0' (graphics) */
    char charset[2];
    int gl;                 /* 0 or 1: which one is in use */
    struct cursor_save saved, saved_alt;

    /* parser */
    int state;
    int params[16], nparams;
    int have_param;
    char priv;              /* '?', '>', '<', '=' or 0 */
    char inter;             /* intermediate byte, or 0 */
    unsigned char utf8[4];
    int utf8_len;
    uint32_t last_char;     /* for REP */

    struct sb sb;           /* scrollback of the main screen */
    int dirty;              /* something changed since the last draw */
    unsigned char *rowdirty;    /* per row: changed since the last draw
                                 * (the drawer clears these) */
    int bell;               /* BEL received */

    /* Answers to queries are written back to the program through this. */
    void (*reply)(void *ctx, const char *s, size_t n);
    void *reply_ctx;
};

/* history_dir may be NULL; see sb_init() for the two sizes. */
void vt_init(struct vt *vt, int rows, int cols, const char *history_dir,
             size_t disk_bytes, size_t ram_bytes);
void vt_free(struct vt *vt);

/* Feed n bytes of program output. */
void vt_write(struct vt *vt, const unsigned char *buf, size_t n);

/* Change the size, keeping the cursor's line on screen. */
void vt_resize(struct vt *vt, int rows, int cols);

/* The cell at (row, col) of the visible screen. */
static inline struct cell *vt_cell(const struct vt *vt, int row, int col)
{
    return &vt->g->c[row * vt->cols + col];
}

#endif /* VT_H */
