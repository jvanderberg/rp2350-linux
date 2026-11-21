/*
 * keys.c - decode terminal input into key codes. See keys.h.
 *
 * Bytes are read in chunks into a small buffer. An Esc that is not
 * followed by another byte within ESC_WAIT_MS is reported as a bare Esc;
 * otherwise the following bytes are parsed as a CSI ("\e[...") or SS3
 * ("\eO.") sequence, an SGR mouse report ("\e[<b;x;yM"), or Alt+key.
 */
#include <errno.h>
#include <poll.h>
#include <unistd.h>

#include "keys.h"
#include "screen.h"

#define ESC_WAIT_MS 100

struct mouse_event key_mouse;

static unsigned char ibuf[64];
static int ilen, ipos;

/* Next input byte; -1 on timeout, -2 if a signal interrupted the wait. */
static int rd(int timeout_ms)
{
    struct pollfd pfd;
    ssize_t n;

    if (ipos < ilen)
        return ibuf[ipos++];
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    pfd.revents = 0;
    n = poll(&pfd, 1, timeout_ms);
    if (n < 0)
        return errno == EINTR ? -2 : -1;
    if (n == 0)
        return -1;
    n = read(STDIN_FILENO, ibuf, sizeof ibuf);
    if (n <= 0) {
        if (n == 0 || (errno != EINTR && errno != EAGAIN))
            scr_quit = 1;   /* the terminal went away */
        return -2;
    }
    ilen = (int)n;
    ipos = 0;
    return ibuf[ipos++];
}

/* Byte inside an escape sequence: short wait, interruptions ignored. */
static int rd_seq(void)
{
    int c;

    do
        c = rd(ESC_WAIT_MS);
    while (c == -2 && !scr_quit);
    return c;
}

/* "\e[<b;x;yM" (press) or "...m" (release); the "\e[<" is consumed. */
static int decode_sgr_mouse(void)
{
    int v[3] = { 0, 0, 0 }, n = 0, c;

    for (;;) {
        c = rd_seq();
        if (c >= '0' && c <= '9') {
            if (n < 3)
                v[n] = v[n] * 10 + (c - '0');
        } else if (c == ';') {
            n++;
        } else {
            break;
        }
    }
    if (c != 'M' && c != 'm')
        return K_NONE;
    if (v[0] & 32)
        return K_NONE;      /* motion: not requested, ignore */
    key_mouse.button = v[0] & ~(4 | 8 | 16);   /* strip shift/alt/ctrl */
    key_mouse.x = v[1] - 1;
    key_mouse.y = v[2] - 1;
    key_mouse.release = c == 'm';
    return K_MOUSE;
}

/* After "\e[": parameters, then a final byte. */
static int decode_csi(void)
{
    int p[4] = { 0, 0, 0, 0 }, np = 0, c;

    c = rd_seq();
    if (c == '<')
        return decode_sgr_mouse();
    if (c == '[') {                 /* Linux console F1..F5: "\e[[A" */
        c = rd_seq();
        return c >= 'A' && c <= 'E' ? K_F(c - 'A' + 1) : K_NONE;
    }
    while ((c >= '0' && c <= '9') || c == ';') {
        if (c == ';') {
            if (np < 3)
                np++;
        } else {
            p[np] = p[np] * 10 + (c - '0');
        }
        c = rd_seq();
    }
    switch (c) {
    case 'A': return K_UP;
    case 'B': return K_DOWN;
    case 'C': return K_RIGHT;
    case 'D': return K_LEFT;
    case 'H': return K_HOME;
    case 'F': return K_END;
    case 'Z': return K_BTAB;
    case 'P': case 'Q': case 'R': case 'S':
        return K_F(c - 'P' + 1);    /* "\e[1;2P" style modified F1-F4 */
    case '~':
        switch (p[0]) {
        case 1: case 7: return K_HOME;
        case 2: return K_INS;
        case 3: return K_DEL;
        case 4: case 8: return K_END;
        case 5: return p[1] == 5 ? K_CTRL_PGUP : K_PGUP;
        case 6: return p[1] == 5 ? K_CTRL_PGDN : K_PGDN;
        case 11: case 12: case 13: case 14: case 15:
            return K_F(p[0] - 10);
        case 17: case 18: case 19: case 20: case 21:
            return K_F(p[0] - 11);
        case 23: case 24:
            return K_F(p[0] - 12);
        }
        return K_NONE;
    }
    return K_NONE;
}

static int decode_escape(void)
{
    int c = rd_seq();

    if (c < 0 || c == 27)
        return K_ESC;               /* lone Esc, or Esc Esc */
    if (c == '[')
        return decode_csi();
    if (c == 'O') {                 /* SS3: vt100 keypad / F1-F4 */
        c = rd_seq();
        switch (c) {
        case 'A': return K_UP;
        case 'B': return K_DOWN;
        case 'C': return K_RIGHT;
        case 'D': return K_LEFT;
        case 'H': return K_HOME;
        case 'F': return K_END;
        case 'P': case 'Q': case 'R': case 'S':
            return K_F(c - 'P' + 1);
        }
        return K_NONE;
    }
    if (c >= '0' && c <= '9')       /* mc: Esc 1 = F1 ... Esc 0 = F10 */
        return K_F(c == '0' ? 10 : c - '0');
    if (c == '\n')
        c = K_ENTER;
    if (c == 8)
        c = K_BS;
    return K_ALT | c;
}

int key_get(int timeout_ms)
{
    int c;

    for (;;) {
        if (scr_quit)
            return K_QUIT;
        if (scr_winch) {
            scr_winch = 0;
            return K_RESIZE;
        }
        c = rd(timeout_ms);
        if (c == -2)
            continue;               /* signal: re-check the flags */
        if (c == -1)
            return K_NONE;
        break;
    }
    if (c == 27)
        return decode_escape();
    if (c == 8)
        return K_BS;
    if (c == '\n')
        return K_ENTER;
    return c;
}
