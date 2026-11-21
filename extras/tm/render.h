/*
 * render.h - turn a composed screen into terminal output, sending only
 * what changed since the previous frame.
 *
 * The server composes each frame (window contents + status line) into a
 * grid of cells; render_frame() compares it with what the attached
 * terminal is known to show and appends the escape sequences that bring
 * it up to date. On a slow link a keystroke usually costs a few bytes.
 */
#ifndef RENDER_H
#define RENDER_H

#include "cell.h"
#include "util.h"

/* What the outer terminal currently shows, and its modes. */
struct term {
    int rows, cols;
    struct cell *front;     /* rows * cols; NULL until the first frame */
    int cursor_on;          /* cursor shown (-1: unknown) */
    int app_cursor, app_keypad, mouse, paste;   /* modes as last sent */
};

/* Forget the terminal's contents (after attach or resize): the next
 * frame repaints everything. */
void term_reset(struct term *t, int rows, int cols);
void term_free(struct term *t);

/* Append to `out` what changes the terminal from its current contents to
 * `scr` (t->rows * t->cols cells), then place the cursor at (cx, cy), or
 * hide it if cy < 0. Only rows with rows[y] set are looked at (all of
 * them if rows is NULL): the others must be unchanged. */
void render_frame(struct buf *out, struct term *t, const struct cell *scr,
                  const unsigned char *rows, int cx, int cy);

/* Append the mode switches that make the outer terminal send keys and
 * mouse events the way the active window expects. */
void render_modes(struct buf *out, struct term *t, int app_cursor,
                  int app_keypad, int mouse, int paste);

#endif /* RENDER_H */
