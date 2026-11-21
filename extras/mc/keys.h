/*
 * keys.h - keyboard and mouse input.
 *
 * key_get() turns the terminal's byte stream (escape sequences from xterm,
 * the Linux console, vt100 and friends) into a single integer per key.
 * Plain characters are returned as their byte value.
 */
#ifndef KEYS_H
#define KEYS_H

enum {
    K_NONE = -1,        /* timeout: nothing was pressed */
    K_TAB = '\t',
    K_ENTER = '\r',
    K_ESC = 27,
    K_BS = 127,

    K_UP = 0x100, K_DOWN, K_LEFT, K_RIGHT,
    K_HOME, K_END, K_PGUP, K_PGDN, K_INS, K_DEL,
    K_CTRL_PGUP, K_CTRL_PGDN,
    K_BTAB,             /* shift-Tab */
    K_MOUSE,            /* details are in key_mouse */
    K_RESIZE,           /* the terminal changed size */
    K_QUIT,             /* SIGTERM/SIGHUP or end of input */

    K_F1 = 0x140        /* K_F1 .. K_F1 + 11 */
};

#define K_F(n)     (K_F1 + (n) - 1)
#define K_ALT      0x1000              /* or-ed with the key: Alt/Meta */
#define K_CTRL(c)  ((c) & 0x1F)

/* The last mouse event (valid when key_get() returned K_MOUSE). */
struct mouse_event {
    int x, y;           /* 0-based screen cell */
    int button;         /* 0 left, 1 middle, 2 right, 64/65 wheel up/down */
    int release;        /* 1 for a button release */
};
extern struct mouse_event key_mouse;

/* Wait up to timeout_ms (-1: forever) for a key. Returns K_NONE on
 * timeout. Like mc, Esc followed by a digit gives F1..F10, which helps on
 * terminals that eat the function keys. */
int key_get(int timeout_ms);

#endif /* KEYS_H */
