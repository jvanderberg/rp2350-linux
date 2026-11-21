/*
 * copy.h - copy mode: scroll back through history, search, select, copy.
 *
 * Keys (vi style, arrows work too):
 *
 *   Up/k Down/j Left/h Right/l   move
 *   PgUp/C-b PgDn/C-f C-u C-d    page / half page
 *   g Home / G End               oldest / newest line
 *   0 / $                        start / end of the line
 *   / and ?                      search down / up;  n and N repeat
 *   Space or v                   start selecting
 *   Enter                        copy the selection and leave
 *   q Esc                        leave
 */
#ifndef COPY_H
#define COPY_H

#include <stddef.h>

#include "window.h"

enum {
    COPY_STAY,              /* key handled, still in copy mode */
    COPY_EXIT,              /* leave copy mode */
    COPY_SEARCH_DOWN,       /* ask for a search string (searching down) */
    COPY_SEARCH_UP          /* ... searching up */
};

/* Enter copy mode on window w, whose screen area has `rows` rows. */
void copy_enter(struct window *w, int rows);

/* Handle one key (keys.h codes). On Enter with a selection, the text is
 * stored in *yank (malloc'd, caller frees) and its length in *yank_len. */
int copy_key(struct window *w, int key, int rows, char **yank,
             size_t *yank_len);

/* Search for `text` from the cursor, down or up; 0 if not found. */
int copy_search(struct window *w, const char *text, int up, int rows);

/* Scroll the view by `lines` (negative: back in history), keeping the
 * cursor on screen. Returns 1 if the view is now at the bottom. */
int copy_scroll(struct window *w, int lines, int rows);

/* Mouse: put the cursor at screen position (x, y). With drag 0 this
 * also drops the selection and anchors a new one there; with drag 1 it
 * extends the selection to (x, y), scrolling when y is off the top or
 * bottom. */
void copy_point(struct window *w, int x, int y, int rows, int drag);

/* Copy the selection, if any, into *yank (as copy_key does on Enter)
 * and leave copy mode. */
void copy_finish(struct window *w, char **yank, size_t *yank_len);

/* Draw the copy-mode view into scr (rows x cols) and give the cursor. */
void copy_draw(struct window *w, struct cell *scr, int rows, int cols,
               int *cx, int *cy);

#endif /* COPY_H */
