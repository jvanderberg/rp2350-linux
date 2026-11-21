/*
 * keys.h - decode keys from a buffer of terminal input.
 *
 * Normally tm passes the client's keystrokes straight to the program in
 * the window. Only in copy mode, prompts and after the prefix key does it
 * need to know which key was pressed; key_next() decodes one from the
 * bytes received.
 */
#ifndef KEYS_H
#define KEYS_H

#include <stddef.h>

enum {
    K_UP = 0x100, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END,
    K_PGUP, K_PGDN, K_INS, K_DEL, K_IGNORE
};

#define K_CTRL(c) ((c) & 0x1F)
#define K_ESC     27
#define K_ENTER   '\r'
#define K_BS      127

/* Decode the key at the start of s[0, n). Returns the key and sets
 * *used to the bytes it took (at least 1). Unknown escape sequences
 * decode to K_IGNORE. */
int key_next(const unsigned char *s, size_t n, size_t *used);

/* A mouse report: SGR encoding (ESC [ < b ; x ; y M/m) or, from
 * terminals without it, X10 (ESC [ M then b, x, y as bytes + 32). b is the
 * terminal's button code: low two bits the button (3: none), plus
 * MB_SHIFT/MB_META/MB_CTRL, MB_MOTION for movement, MB_WHEEL for the
 * wheel (button 0 up, 1 down). x and y count from 0. */
#define MB_SHIFT  4
#define MB_META   8
#define MB_CTRL   16
#define MB_MOTION 32
#define MB_WHEEL  64

struct mouse {
    int b, x, y;
    int release;
};

/* If s starts with a mouse report, decode it: returns 1 and sets *used;
 * 0 if it is not one; -1 if it is cut off (wait for more bytes). */
int mouse_parse(const unsigned char *s, size_t n, struct mouse *m,
                size_t *used);

#endif /* KEYS_H */
