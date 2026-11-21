/*
 * screen.h - off-screen cell buffer and terminal control.
 *
 * Everything is drawn into a back buffer of character cells. scr_flush()
 * compares it with what the terminal is known to show and sends only the
 * cells that changed. That keeps a full repaint cheap on a slow serial or
 * network link, so the rest of the program can simply redraw the whole
 * screen after every key press without worrying about cost.
 *
 * Coordinates are 0-based (row y, column x). Drawing outside the screen is
 * silently clipped, so callers never need to range-check.
 */
#ifndef SCREEN_H
#define SCREEN_H

#include <signal.h>

/* The 16 classic PC colours that mc's skins are written in, plus the
 * terminal's own default colour (used for the hint and command lines). */
enum {
    C_BLACK, C_RED, C_GREEN, C_BROWN, C_BLUE, C_MAGENTA, C_CYAN, C_LIGHTGRAY,
    C_GRAY, C_BRIGHTRED, C_BRIGHTGREEN, C_YELLOW, C_BRIGHTBLUE,
    C_BRIGHTMAGENTA, C_BRIGHTCYAN, C_WHITE,
    C_DEFAULT
};

/* An attribute is a foreground/background colour pair packed in an int. */
#define ATTR(fg, bg)  ((fg) | ((bg) << 5))
#define ATTR_FG(a)    ((a) & 31)
#define ATTR_BG(a)    (((a) >> 5) & 31)

/* Box-drawing characters (Unicode code points). */
#define BOX_H   0x2500  /* ─ */
#define BOX_V   0x2502  /* │ */
#define BOX_TL  0x250C  /* ┌ */
#define BOX_TR  0x2510  /* ┐ */
#define BOX_BL  0x2514  /* └ */
#define BOX_BR  0x2518  /* ┘ */
#define BOX_LT  0x251C  /* ├ */
#define BOX_RT  0x2524  /* ┤ */

/* Flags for scr_text(): alignment, and how to shorten text that is too
 * long for its field. */
#define T_LEFT        0
#define T_RIGHT       1
#define T_CENTER      2
#define T_TRIM_MID    4   /* "long_na~me.txt": mc's style for file names */
#define T_TRIM_START  8   /* "...tail/of/path": mc's style for paths     */

extern int scr_rows, scr_cols;

/* Set from signal handlers; key_get() turns them into key codes. */
extern volatile sig_atomic_t scr_winch;  /* terminal was resized    */
extern volatile sig_atomic_t scr_quit;   /* SIGTERM/SIGHUP received */

int  scr_open(void);         /* raw mode, alternate screen, mouse on */
void scr_close(void);        /* undo scr_open(); safe to call twice */
void scr_suspend(void);      /* give the terminal back to a child program */
void scr_resume(void);       /* take it back and schedule a full repaint */
int  scr_update_size(void);  /* re-read the size; 1 if it changed */
void scr_invalidate(void);   /* forget what the terminal shows (Ctrl-L) */

void scr_put(int y, int x, unsigned ch, int attr);
int  scr_puts(int y, int x, const char *s, int attr);  /* returns end x */
void scr_text(int y, int x, int w, const char *s, int attr, int how);
void scr_fill(int y, int x, int h, int w, unsigned ch, int attr);
void scr_hline(int y, int x, int w, unsigned ch, int attr);
void scr_box(int y, int x, int h, int w, int attr);
void scr_shadow(int y, int x, int h, int w);
void scr_cursor(int y, int x);  /* where to leave the cursor; y < 0 hides */
void scr_flush(void);

#endif /* SCREEN_H */
