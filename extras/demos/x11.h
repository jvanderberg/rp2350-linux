// SPDX-License-Identifier: MIT
/*
 * x11.h - a tiny X11 client for the demos, the window counterpart of term.h.
 *
 * It speaks the X11 protocol directly over a socket, so there is no Xlib to
 * build or link: include this file and compile with cc. The board is the
 * client; the screen belongs to an X server elsewhere, such as XQuartz on a
 * Mac.
 *
 *     if (x_open(NULL) < 0)                 // NULL: use $DISPLAY (HOST:0)
 *         return 1;
 *     x_window("demo", 320, 200);
 *     for (;;) {
 *         struct x_event e;
 *
 *         while (x_event(&e))               // never blocks
 *             if (e.type == XE_CLOSE || (e.type == XE_KEY_DOWN && e.key == 'q'))
 *                 return 0;
 *         x_color(x_rgb(0, 0, 80));
 *         x_fill_rect(0, 0, x_width, x_height);
 *         x_color(x_rgb(255, 255, 255));
 *         x_text(10, 20, "hello");
 *         x_present();                      // show the frame
 *         x_wait(33);                       // until input arrives, or 33 ms
 *     }
 *
 * Drawing goes to a back buffer the size of the window and appears only at
 * x_present(), so frames never flicker; when the window is uncovered the
 * back buffer is copied again without asking the program.
 *
 *   colours   x_rgb(r, g, b) makes a pixel value; x_color() sets the colour
 *             for the drawing calls below. x_line_width() sets line width.
 *   shapes    x_fill_rect, x_rect, x_line, x_fill_ellipse, x_ellipse
 *             (ellipses by bounding box, like rectangles)
 *   text      x_text(x, y, s): y is the baseline; the server's "fixed"
 *             font, usually 6x13 pixels per character
 *   batches   x_fill_rects, x_segments: many in one request; x_fill_poly
 *             for a convex polygon. Arrays of shorts (see each function).
 *   images    x_image(x, y, w, h, pixels, stride): 32-bit pixels made by
 *             x_rgb, stride in pixels. Costly over a slow link: 256 KB for
 *             a 320x200 frame, where shapes cost a few bytes each.
 *   canvases  x_canvas(w, h) makes an off-screen image; x_target(c) sends
 *             the drawing calls there (0: back to the frame); x_blit(c, ...)
 *             copies part of it into the frame. Draw something once, then
 *             reuse it for 28 bytes a frame.
 *   masks     x_mask(w, h) makes a 1-bit image, all clear; x_mask_rects
 *             sets rectangles of it. x_blit_masked copies only the pixels
 *             a mask has set, for shapes that aren't rectangles. For a mask
 *             used again and again, make a stencil of it once (x_stencil)
 *             and copy with x_blit_stencil: the server does the costly part
 *             of a masked copy once instead of every time.
 *             x_fill_stippled fills a rectangle only where a small mask,
 *             repeated, is set: a dither, for a see-through tint.
 *             x_target_window() draws straight onto the window, over what
 *             x_present() last showed, leaving the back buffer alone.
 *   input     x_event() returns key down and up (a real release, unlike a
 *             terminal; a held key repeats as up/down pairs), mouse buttons
 *             and motion, resize (draw again) and close. e.key is ASCII
 *             for printable keys, else an X_KEY_* code.
 *             x_pointer() asks where the mouse is, even outside the window.
 *   other     x_sync() waits until the server has drawn everything sent
 *             (call it after x_present() so a slow link can't build up a
 *             queue of old frames), x_wait(ms), x_flush(), x_fd() for your
 *             own poll(), x_close()
 *   fences    x_fence() after x_present(), and x_fence_wait() before the
 *             next one, instead of x_sync(): the program draws the next
 *             frame while the last one travels, and still never more than
 *             one frame ahead of the server
 *
 * Define X11_TIMING before including this to count the time spent sending
 * in x_write_us (microseconds).
 *
 * Needs a TrueColor display (any current server, XQuartz included) that
 * lets this machine connect: on the machine with the screen, allow network
 * clients (XQuartz: Settings > Security) and run `xhost +THIS-ADDRESS`.
 * Xauthority cookies are not supported.
 */
#ifndef DEMO_X11_H
#define DEMO_X11_H

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

enum {
    XE_NONE, XE_KEY_DOWN, XE_KEY_UP, XE_BUTTON_DOWN, XE_BUTTON_UP,
    XE_MOTION, XE_RESIZE, XE_CLOSE
};

/* X keysyms for keys that aren't printable characters */
#define X_KEY_BACKSPACE 0xff08
#define X_KEY_TAB       0xff09
#define X_KEY_RETURN    0xff0d
#define X_KEY_ESCAPE    0xff1b
#define X_KEY_HOME      0xff50
#define X_KEY_LEFT      0xff51
#define X_KEY_UP        0xff52
#define X_KEY_RIGHT     0xff53
#define X_KEY_DOWN      0xff54
#define X_KEY_PAGE_UP   0xff55
#define X_KEY_PAGE_DOWN 0xff56
#define X_KEY_END       0xff57
#define X_KEY_F1        0xffbe          /* F2.. follow on */
#define X_KEY_SHIFT_L   0xffe1
#define X_KEY_SHIFT_R   0xffe2
#define X_KEY_CONTROL_L 0xffe3
#define X_KEY_CONTROL_R 0xffe4
#define X_KEY_DELETE    0xffff

struct x_event {
    int type;                           /* XE_* */
    int key;                            /* keys: ASCII or X_KEY_* */
    int button;                         /* buttons: 1 left, 2 middle, 3 right, 4/5 wheel */
    int x, y;                           /* keys, buttons, motion: mouse in the window */
};

static int x_width, x_height;           /* the window's size now */
static unsigned long long x_sent;       /* bytes sent to the X server so far */

/* ---- internals ---- */

static int xi_fd = -1, xi_dead;
static unsigned char xi_out[16384];
static size_t xi_out_len;
static unsigned int xi_seq;             /* sequence number of the last request */
static unsigned int xi_id_base, xi_id_next;
static unsigned int xi_root, xi_depth, xi_bpp, xi_max_req;
static unsigned int xi_rmask, xi_gmask, xi_bmask;
static int xi_image_msb;                /* server wants image bytes big-endian */
static unsigned int xi_win, xi_back, xi_gc, xi_font, xi_color = 0xffffffffu;
static unsigned int xi_target;          /* canvas the drawing calls go to; 0: the frame */
static unsigned int xi_wm_protocols, xi_wm_delete;
static int xi_min_kc, xi_kc_count, xi_syms_per;
static unsigned int *xi_keysyms;
static struct x_event xi_queue[64];
static int xi_qhead, xi_qlen;

static unsigned int xi_get16(const unsigned char *p) { return p[0] | (unsigned int)p[1] << 8; }
static unsigned int xi_get32(const unsigned char *p) { return xi_get16(p) | xi_get16(p + 2) << 16; }

static void xi_push(int type, int key, int button, int x, int y)
{
    struct x_event *e;

    if (xi_qlen == (int)(sizeof xi_queue / sizeof xi_queue[0]))
        return;                         /* full: drop it */
    e = &xi_queue[(xi_qhead + xi_qlen++) % (int)(sizeof xi_queue / sizeof xi_queue[0])];
    e->type = type;
    e->key = key;
    e->button = button;
    e->x = x;
    e->y = y;
}

/* The connection is gone: say so once, then ignore everything. */
static void xi_lost(void)
{
    if (!xi_dead) {
        xi_dead = 1;
        xi_push(XE_CLOSE, 0, 0, 0, 0);
    }
}

#ifdef X11_TIMING
#include <time.h>
static unsigned long long x_write_us;   /* time spent in write() */
static unsigned long long xi_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000u + (unsigned long long)ts.tv_nsec / 1000u;
}
#endif

static void xi_write(const void *buf, size_t n)
{
    const unsigned char *p = buf;

    while (n > 0 && !xi_dead) {
#ifdef X11_TIMING
        unsigned long long t = xi_us();
        ssize_t w = write(xi_fd, p, n);

        x_write_us += xi_us() - t;
#else
        ssize_t w = write(xi_fd, p, n);
#endif

        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            xi_lost();
            return;
        }
        p += w;
        n -= (size_t)w;
        x_sent += (unsigned long long)w;
    }
}

static inline void x_flush(void)
{
    xi_write(xi_out, xi_out_len);
    xi_out_len = 0;
}

static void xi_put8(unsigned int v) { xi_out[xi_out_len++] = (unsigned char)v; }
static void xi_put16(unsigned int v) { xi_put8(v & 255); xi_put8(v >> 8 & 255); }
static void xi_put32(unsigned int v) { xi_put16(v & 0xffff); xi_put16(v >> 16); }

/* Start a request len 4-byte words long; at most sizeof xi_out is buffered. */
static void xi_req(int opcode, int data, unsigned int len)
{
    if (xi_out_len + len * 4 > sizeof xi_out)
        x_flush();
    xi_put8((unsigned int)opcode);
    xi_put8((unsigned int)data);
    xi_put16(len);
    xi_seq++;
}

static int xi_read(unsigned char *buf, size_t n)
{
    size_t off = 0;

    while (off < n && !xi_dead) {
        ssize_t r = read(xi_fd, buf + off, n - off);

        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            xi_lost();
            return -1;
        }
        off += (size_t)r;
    }
    return xi_dead ? -1 : 0;
}

static unsigned int xi_new_id(void) { return xi_id_base | xi_id_next++; }

static int xi_keysym(int keycode, int state)
{
    unsigned int *ks;
    unsigned int lo, hi;

    if (xi_keysyms == NULL || keycode < xi_min_kc || keycode >= xi_min_kc + xi_kc_count)
        return 0;
    ks = xi_keysyms + (keycode - xi_min_kc) * xi_syms_per;
    lo = ks[0];
    hi = xi_syms_per > 1 && ks[1] != 0 ? ks[1] : lo;
    if (lo >= 'a' && lo <= 'z' && hi == lo)
        hi = lo - 'a' + 'A';
    if (lo >= 'a' && lo <= 'z' && (state & 2))      /* Caps Lock */
        return (int)((state & 1) ? lo : hi);
    return (int)((state & 1) ? hi : lo);             /* Shift */
}

static void xi_copy_back(void)
{
    xi_req(62, 0, 7);                   /* CopyArea */
    xi_put32(xi_back);
    xi_put32(xi_win);
    xi_put32(xi_gc);
    xi_put32(0);
    xi_put32(0);
    xi_put16((unsigned int)x_width);
    xi_put16((unsigned int)x_height);
}

static void xi_make_back(void)
{
    if (xi_back != 0) {
        xi_req(54, 0, 2);               /* FreePixmap */
        xi_put32(xi_back);
    }
    xi_back = xi_new_id();
    xi_req(53, (int)xi_depth, 4);       /* CreatePixmap */
    xi_put32(xi_back);
    xi_put32(xi_win);
    xi_put16((unsigned int)x_width);
    xi_put16((unsigned int)x_height);
}

static unsigned int xi_fence_seq;
static int xi_fence_on;                 /* a fence's reply is still to come */

static void xi_handle(const unsigned char *b)
{
    int x = (short)xi_get16(b + 24), y = (short)xi_get16(b + 26);

    if (xi_fence_on && (b[0] & 0x7f) <= 1 && xi_get16(b + 2) == xi_fence_seq) {
        xi_fence_on = 0;                /* the fence's reply (or error) */
        return;
    }
    switch (b[0] & 0x7f) {
    case 0:
        fprintf(stderr, "x11: error %d from request %d\n", b[1], b[10]);
        break;
    case 2:
        xi_push(XE_KEY_DOWN, xi_keysym(b[1], (int)xi_get16(b + 28)), 0, x, y);
        break;
    case 3:
        xi_push(XE_KEY_UP, xi_keysym(b[1], (int)xi_get16(b + 28)), 0, x, y);
        break;
    case 4:
        xi_push(XE_BUTTON_DOWN, 0, b[1], x, y);
        break;
    case 5:
        xi_push(XE_BUTTON_UP, 0, b[1], x, y);
        break;
    case 6:
        xi_push(XE_MOTION, 0, 0, x, y);
        break;
    case 12:                            /* Expose: show the back buffer again */
        if (xi_get16(b + 16) == 0 && xi_back != 0) {
            xi_copy_back();
            x_flush();
        }
        break;
    case 22: {                          /* ConfigureNotify */
        int w = (int)xi_get16(b + 20), h = (int)xi_get16(b + 22);

        if (w != x_width || h != x_height) {
            x_width = w;
            x_height = h;
            xi_make_back();
            xi_push(XE_RESIZE, 0, 0, w, h);
        }
        break;
    }
    case 33:                            /* ClientMessage: the close button */
        if (b[1] == 32 && xi_get32(b + 8) == xi_wm_protocols &&
            xi_get32(b + 12) == xi_wm_delete)
            xi_push(XE_CLOSE, 0, 0, 0, 0);
        break;
    }
}

/* Read one message. A reply's extra data goes to *extra (malloc'd) if extra
 * is not NULL, otherwise it is skipped. Returns the message type, or -1. */
static int xi_message(unsigned char *b, unsigned char **extra)
{
    size_t n;

    if (xi_read(b, 32) < 0)
        return -1;
    n = ((b[0] & 0x7f) == 1 || (b[0] & 0x7f) == 35) ? (size_t)xi_get32(b + 4) * 4 : 0;
    if (extra != NULL)
        *extra = NULL;
    if (n > 0) {
        unsigned char *p = malloc(n);

        if (p == NULL || xi_read(p, n) < 0) {
            free(p);
            return -1;
        }
        if (extra != NULL)
            *extra = p;
        else
            free(p);
    }
    return b[0] & 0x7f;
}

/* Send what is buffered and wait for the reply to the last request; events
 * that arrive first are queued. Returns 0, or -1 on an error or lost link. */
static int xi_roundtrip(unsigned char *b, unsigned char **extra)
{
    /* Remember which reply: handling an event below can send requests
     * (repainting after Expose, a new back buffer after a resize). */
    unsigned int want = xi_seq & 0xffff;

    x_flush();
    for (;;) {
        int t = xi_message(b, extra);

        if (t < 0)
            return -1;
        if ((t == 0 || t == 1) && xi_get16(b + 2) == want) {
            if (t == 0) {
                xi_handle(b);
                return -1;
            }
            return 0;
        }
        if (extra != NULL) {
            free(*extra);
            *extra = NULL;
        }
        xi_handle(b);
    }
}

static unsigned int xi_atom(const char *name)
{
    unsigned char b[32];
    unsigned int n = (unsigned int)strlen(name);

    xi_req(16, 0, 2 + (n + 3) / 4);     /* InternAtom */
    xi_put16(n);
    xi_put16(0);
    for (unsigned int i = 0; i < n; i++)
        xi_put8((unsigned char)name[i]);
    while (n++ & 3)
        xi_put8(0);
    return xi_roundtrip(b, NULL) < 0 ? 0 : xi_get32(b + 8);
}

static int xi_connect_unix(const char *path)
{
    struct sockaddr_un sa;
    int s;

    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof sa.sun_path || (s = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
        return -1;
    strcpy(sa.sun_path, path);
    if (connect(s, (struct sockaddr *)&sa, sizeof sa) < 0) {
        close(s);
        return -1;
    }
    return s;
}

static int xi_connect_tcp(const char *host, int port)
{
    struct addrinfo hints, *res, *ai;
    char service[16];
    int s = -1, one = 1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof service, "%d", port);
    if (getaddrinfo(host, service, &hints, &res) != 0)
        return -1;
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        if ((s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) < 0)
            continue;
        if (connect(s, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(s);
        s = -1;
    }
    freeaddrinfo(res);
    if (s >= 0)
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return s;
}

/* DISPLAY is HOST:N[.S] (TCP port 6000+N), :N or unix:N (the local socket),
 * or a socket path such as XQuartz's /private/tmp/.../org.xquartz:0. */
static int xi_connect(const char *d)
{
    char host[256], path[64];
    const char *colon = strrchr(d, ':');
    size_t hl;

    if (d[0] == '/')
        return xi_connect_unix(d);
    if (colon == NULL || (hl = (size_t)(colon - d)) >= sizeof host)
        return -1;
    memcpy(host, d, hl);
    host[hl] = '\0';
    if (hl == 0 || strcmp(host, "unix") == 0) {
        snprintf(path, sizeof path, "/tmp/.X11-unix/X%d", atoi(colon + 1));
        return xi_connect_unix(path);
    }
    return xi_connect_tcp(host, 6000 + atoi(colon + 1));
}

/* Pick what we need out of the set-up reply. */
static int xi_parse_setup(const unsigned char *b, size_t len)
{
    const unsigned char *s, *p, *end = b + len;
    unsigned int nformats = b[21], visual;

    xi_id_base = xi_get32(b + 4);
    xi_max_req = xi_get16(b + 18);
    xi_image_msb = b[22];
    xi_min_kc = b[26];
    xi_kc_count = b[27] - b[26] + 1;
    p = b + 32 + ((xi_get16(b + 16) + 3) & ~3u);    /* after the vendor string */
    s = p + 8 * nformats;                           /* the first screen */
    if (s + 40 > end)
        return -1;
    xi_root = xi_get32(s);
    visual = xi_get32(s + 32);
    xi_depth = s[38];
    for (unsigned int i = 0; i < nformats; i++)
        if (p[i * 8] == xi_depth)
            xi_bpp = p[i * 8 + 1];
    /* find the root visual among the screen's depths */
    p = s + 40;
    for (int d = 0; d < s[39] && p + 8 <= end; d++) {
        int nv = (int)xi_get16(p + 2);

        p += 8;
        for (int v = 0; v < nv && p + 24 <= end; v++, p += 24)
            if (xi_get32(p) == visual) {
                if (p[4] != 4)          /* TrueColor */
                    return -1;
                xi_rmask = xi_get32(p + 8);
                xi_gmask = xi_get32(p + 12);
                xi_bmask = xi_get32(p + 16);
            }
    }
    return xi_rmask != 0 ? 0 : -1;
}

/* ---- the API ---- */

/* Connect to display (NULL: $DISPLAY). Prints the reason and returns -1 on failure. */
static inline int x_open(const char *display)
{
    unsigned char head[8], b[32], *setup, *syms = NULL;
    size_t len;
    const char *d = display != NULL ? display : getenv("DISPLAY");

    if (d == NULL || *d == '\0') {
        fprintf(stderr, "x11: set DISPLAY to the X server, e.g. DISPLAY=192.168.1.2:0\n");
        return -1;
    }
    if ((xi_fd = xi_connect(d)) < 0) {
        fprintf(stderr, "x11: can't connect to the X server at %s\n", d);
        return -1;
    }
    xi_put8('l');                       /* little-endian, protocol 11.0, no auth */
    xi_put8(0);
    xi_put16(11);
    xi_put16(0);
    xi_put16(0);
    xi_put16(0);
    xi_put16(0);
    x_flush();
    if (xi_read(head, 8) < 0) {
        fprintf(stderr, "x11: the X server at %s hung up\n", d);
        return -1;
    }
    len = (size_t)xi_get16(head + 6) * 4;
    setup = malloc(len ? len : 1);
    if (setup == NULL || xi_read(setup, len) < 0) {
        fprintf(stderr, "x11: no set-up reply from %s\n", d);
        return -1;
    }
    if (head[0] != 1) {
        fprintf(stderr, "x11: %s refused the connection: %.*s\n"
                "x11: on that machine, try: xhost +ADDRESS-OF-THIS-ONE\n",
                d, head[0] == 0 ? head[1] : (int)len, (char *)setup);
        free(setup);
        return -1;
    }
    if (xi_parse_setup(setup, len) < 0 || xi_bpp != 32) {
        fprintf(stderr, "x11: %s is not a 24/32-bit TrueColor display\n", d);
        free(setup);
        return -1;
    }
    free(setup);

    xi_req(101, 0, 2);                  /* GetKeyboardMapping */
    xi_put8((unsigned int)xi_min_kc);
    xi_put8((unsigned int)xi_kc_count);
    xi_put16(0);
    if (xi_roundtrip(b, &syms) == 0 && syms != NULL) {
        size_t n = (size_t)xi_get32(b + 4);

        xi_syms_per = b[1];
        xi_keysyms = malloc(n * sizeof *xi_keysyms);
        if (xi_keysyms != NULL)
            for (size_t i = 0; i < n; i++)
                xi_keysyms[i] = xi_get32(syms + i * 4);
    }
    free(syms);

    xi_font = xi_new_id();
    xi_req(45, 0, 3 + 2);               /* OpenFont "fixed" */
    xi_put32(xi_font);
    xi_put16(5);
    xi_put16(0);
    xi_put32('f' | 'i' << 8 | 'x' << 16 | (unsigned int)'e' << 24);
    xi_put32('d');
    xi_wm_protocols = xi_atom("WM_PROTOCOLS");
    xi_wm_delete = xi_atom("WM_DELETE_WINDOW");
    return xi_dead ? -1 : 0;
}

static inline void x_close(void)
{
    if (xi_fd >= 0) {
        x_flush();
        close(xi_fd);                   /* the server removes the window */
    }
    xi_fd = -1;
    xi_dead = 1;
    free(xi_keysyms);
    xi_keysyms = NULL;
}

/* A pixel value for a colour, each component 0-255. */
static inline unsigned int x_rgb(int r, int g, int b)
{
    unsigned int c[3] = { (unsigned int)r & 255, (unsigned int)g & 255, (unsigned int)b & 255 };
    unsigned int m[3] = { xi_rmask, xi_gmask, xi_bmask }, v = 0;

    for (int i = 0; i < 3; i++) {
        unsigned int mask = m[i], shift = 0, bits = 0;

        while (mask != 0 && !(mask & 1)) {
            mask >>= 1;
            shift++;
        }
        while (mask & 1) {
            mask >>= 1;
            bits++;
        }
        v |= (bits >= 8 ? c[i] << (bits - 8) : c[i] >> (8 - bits)) << shift;
    }
    return v;
}

/* Open the program's one window and show it; it starts out black. */
static inline unsigned int x_window(const char *title, int w, int h)
{
    unsigned int n = (unsigned int)strlen(title);

    x_width = w;
    x_height = h;
    xi_win = xi_new_id();
    xi_req(1, 0, 8 + 2);                /* CreateWindow, depth from the parent */
    xi_put32(xi_win);
    xi_put32(xi_root);
    xi_put16(0);
    xi_put16(0);
    xi_put16((unsigned int)w);
    xi_put16((unsigned int)h);
    xi_put16(0);                        /* border */
    xi_put16(1);                        /* InputOutput */
    xi_put32(0);                        /* visual from the parent */
    xi_put32(0x2 | 0x800);              /* background-pixel, event-mask */
    xi_put32(x_rgb(0, 0, 0));
    xi_put32(0x1 | 0x2 | 0x4 | 0x8 | 0x40 | 0x8000 | 0x20000);
    /* keys, buttons, motion, exposure, structure */

    xi_req(18, 0, 6 + (n + 3) / 4);     /* ChangeProperty WM_NAME */
    xi_put32(xi_win);
    xi_put32(39);                       /* WM_NAME */
    xi_put32(31);                       /* STRING */
    xi_put32(8);                        /* format 8, 3 unused */
    xi_put32(n);
    for (unsigned int i = 0; i < n; i++)
        xi_put8((unsigned char)title[i]);
    while (n++ & 3)
        xi_put8(0);

    if (xi_wm_protocols != 0 && xi_wm_delete != 0) {
        xi_req(18, 0, 6 + 1);           /* WM_PROTOCOLS = WM_DELETE_WINDOW */
        xi_put32(xi_win);
        xi_put32(xi_wm_protocols);
        xi_put32(4);                    /* ATOM */
        xi_put32(32);
        xi_put32(1);
        xi_put32(xi_wm_delete);
    }

    xi_gc = xi_new_id();
    xi_req(55, 0, 4 + 3);               /* CreateGC */
    xi_put32(xi_gc);
    xi_put32(xi_win);
    xi_put32(0x4 | 0x4000 | 0x10000);   /* foreground, font, graphics-exposures */
    xi_put32(x_rgb(255, 255, 255));
    xi_put32(xi_font);
    xi_put32(0);
    xi_color = x_rgb(255, 255, 255);

    xi_make_back();
    xi_req(8, 0, 2);                    /* MapWindow */
    xi_put32(xi_win);
    x_flush();
    return xi_win;
}

static void xi_change_gc(unsigned int mask, unsigned int value)
{
    xi_req(56, 0, 3 + 1);               /* ChangeGC */
    xi_put32(xi_gc);
    xi_put32(mask);
    xi_put32(value);
}

static inline void x_color(unsigned int pixel)
{
    if (pixel != xi_color) {
        xi_change_gc(0x4, pixel);
        xi_color = pixel;
    }
}

static inline void x_line_width(int w)
{
    xi_change_gc(0x10, (unsigned int)w);
}

static unsigned int xi_dst(void) { return xi_target != 0 ? xi_target : xi_back; }

static void xi_box(int opcode, int x, int y, int w, int h, int arc)
{
    if (w <= 0 || h <= 0)
        return;
    xi_req(opcode, 0, arc ? 3 + 3 : 3 + 2);
    xi_put32(xi_dst());
    xi_put32(xi_gc);
    xi_put16((unsigned int)x);
    xi_put16((unsigned int)y);
    xi_put16((unsigned int)w);
    xi_put16((unsigned int)h);
    if (arc) {
        xi_put16(0);                    /* from 0 degrees */
        xi_put16(360 * 64);             /* all the way round, in 1/64 degree */
    }
}

static inline void x_fill_rect(int x, int y, int w, int h) { xi_box(70, x, y, w, h, 0); }
static inline void x_rect(int x, int y, int w, int h) { xi_box(67, x, y, w, h, 0); }
static inline void x_fill_ellipse(int x, int y, int w, int h) { xi_box(71, x, y, w, h, 1); }
static inline void x_ellipse(int x, int y, int w, int h) { xi_box(68, x, y, w, h, 1); }

static inline void x_line(int x1, int y1, int x2, int y2)
{
    xi_req(66, 0, 3 + 2);               /* PolySegment */
    xi_put32(xi_dst());
    xi_put32(xi_gc);
    xi_put16((unsigned int)x1);
    xi_put16((unsigned int)y1);
    xi_put16((unsigned int)x2);
    xi_put16((unsigned int)y2);
}

/* Text in the current colour; (x, y) is the left end of the baseline. */
static inline void x_text(int x, int y, const char *s)
{
    unsigned int n = (unsigned int)strlen(s);

    if (n == 0)
        return;
    if (n > 254)
        n = 254;
    xi_req(74, 0, 4 + (n + 2 + 3) / 4); /* PolyText8 */
    xi_put32(xi_dst());
    xi_put32(xi_gc);
    xi_put16((unsigned int)x);
    xi_put16((unsigned int)y);
    xi_put8(n);                         /* one text item: length, delta, chars */
    xi_put8(0);
    for (unsigned int i = 0; i < n; i++)
        xi_put8((unsigned char)s[i]);
    for (n += 2; n & 3; n++)
        xi_put8(0);
}

/* Copy w x h pixels (from x_rgb) to (x, y); stride is the row length in pixels.
 * Large images go in several requests, rows at a time. */
static inline void x_image(int x, int y, int w, int h, const unsigned int *pixels, int stride)
{
    unsigned int one = 1;
    int native = (*(unsigned char *)&one == 1) != xi_image_msb;
    int rows_max;

    if (w <= 0 || h <= 0)
        return;
    rows_max = (int)((xi_max_req * 4 - 24) / ((unsigned int)w * 4));
    if (rows_max < 1)
        return;                         /* wider than one request can hold */
    while (h > 0 && !xi_dead) {
        int rows = h < rows_max ? h : rows_max;

        xi_req(72, 2, 6 + (unsigned int)(w * rows));   /* PutImage, ZPixmap */
        xi_put32(xi_dst());
        xi_put32(xi_gc);
        xi_put16((unsigned int)w);
        xi_put16((unsigned int)rows);
        xi_put16((unsigned int)x);
        xi_put16((unsigned int)y);
        xi_put8(0);                     /* left pad */
        xi_put8(xi_depth);
        xi_put16(0);
        x_flush();
        for (int r = 0; r < rows; r++) {
            const unsigned int *row = pixels + (size_t)r * (size_t)stride;

            if (native) {
                xi_write(row, (size_t)w * 4);
            } else {
                for (int i = 0; i < w; i++) {
                    unsigned int v = row[i];

                    if (xi_out_len + 4 > sizeof xi_out)
                        x_flush();
                    xi_put8(v >> 24);
                    xi_put8(v >> 16 & 255);
                    xi_put8(v >> 8 & 255);
                    xi_put8(v & 255);
                }
            }
        }
        pixels += (size_t)rows * (size_t)stride;
        y += rows;
        h -= rows;
    }
}

/* Many shapes of one kind in as few requests as fit the buffer: each item
 * is per shorts from v (4 for rectangles and segments). */
static void xi_batch(int opcode, const short *v, int n, int per)
{
    int most = (int)((sizeof xi_out - 12) / ((size_t)per * 2));

    while (n > 0) {
        int k = n < most ? n : most;

        xi_req(opcode, 0, 3 + (unsigned int)(k * per / 2));
        xi_put32(xi_dst());
        xi_put32(xi_gc);
        for (int i = 0; i < k * per; i++)
            xi_put16((unsigned int)v[i]);
        v += k * per;
        n -= k;
    }
}

/* n rectangles: x, y, w, h each. */
static inline void x_fill_rects(const short *r, int n) { xi_batch(70, r, n, 4); }

/* n line segments: x1, y1, x2, y2 each. */
static inline void x_segments(const short *s, int n) { xi_batch(66, s, n, 4); }

/* A filled convex polygon of n points: x, y each. */
static inline void x_fill_poly(const short *pts, int n)
{
    if (n < 3 || n > 1000)
        return;
    xi_req(69, 0, 4 + (unsigned int)n);  /* FillPoly */
    xi_put32(xi_dst());
    xi_put32(xi_gc);
    xi_put8(2);                         /* Convex */
    xi_put8(0);                         /* coordinates relative to the origin */
    xi_put16(0);
    for (int i = 0; i < 2 * n; i++)
        xi_put16((unsigned int)pts[i]);
}

/* An off-screen image w x h; its contents start undefined, so fill it. */
static inline unsigned int x_canvas(int w, int h)
{
    unsigned int c = xi_new_id();

    xi_req(53, (int)xi_depth, 4);       /* CreatePixmap */
    xi_put32(c);
    xi_put32(xi_win);
    xi_put16((unsigned int)(w > 0 ? w : 1));
    xi_put16((unsigned int)(h > 0 ? h : 1));
    return c;
}

static inline void x_free_canvas(unsigned int c)
{
    if (c == 0)
        return;
    if (xi_target == c)
        xi_target = 0;
    xi_req(54, 0, 2);                   /* FreePixmap */
    xi_put32(c);
}

/* Where the drawing calls go: a canvas, or 0 for the frame. */
static inline void x_target(unsigned int c) { xi_target = c; }

/* Draw straight onto the window, over the last frame x_present showed;
 * the back buffer is left as it was. x_target(0) goes back to it. */
static inline void x_target_window(void) { xi_target = xi_win; }

/* Copy w x h from (sx, sy) of canvas c to (dx, dy) of the current target. */
static inline void x_blit(unsigned int c, int sx, int sy, int w, int h, int dx, int dy)
{
    if (w <= 0 || h <= 0)
        return;
    xi_req(62, 0, 7);                   /* CopyArea */
    xi_put32(c);
    xi_put32(xi_dst());
    xi_put32(xi_gc);
    xi_put16((unsigned int)sx);
    xi_put16((unsigned int)sy);
    xi_put16((unsigned int)dx);
    xi_put16((unsigned int)dy);
    xi_put16((unsigned int)w);
    xi_put16((unsigned int)h);
}

/* ---- masks: 1-bit images that say which pixels of a canvas to copy ---- */

static unsigned int xi_gc1, xi_gc1_fg = 2;

static void xi_mask_fg(unsigned int m, unsigned int fg)
{
    if (xi_gc1 == 0) {
        xi_gc1 = xi_new_id();
        xi_req(55, 0, 4 + 1);           /* CreateGC, for depth-1 drawables */
        xi_put32(xi_gc1);
        xi_put32(m);
        xi_put32(0x10000);              /* graphics-exposures */
        xi_put32(0);
    }
    if (fg != xi_gc1_fg) {
        xi_req(56, 0, 3 + 1);           /* ChangeGC: foreground */
        xi_put32(xi_gc1);
        xi_put32(0x4);
        xi_put32(fg);
        xi_gc1_fg = fg;
    }
}

/* A w x h mask, all clear (nothing copied). Free it with x_free_canvas. */
static inline unsigned int x_mask(int w, int h)
{
    unsigned int m = xi_new_id();

    xi_req(53, 1, 4);                   /* CreatePixmap, depth 1 */
    xi_put32(m);
    xi_put32(xi_win);
    xi_put16((unsigned int)(w > 0 ? w : 1));
    xi_put16((unsigned int)(h > 0 ? h : 1));
    xi_mask_fg(m, 0);
    xi_req(70, 0, 3 + 2);               /* PolyFillRectangle: clear it */
    xi_put32(m);
    xi_put32(xi_gc1);
    xi_put32(0);
    xi_put16((unsigned int)(w > 0 ? w : 1));
    xi_put16((unsigned int)(h > 0 ? h : 1));
    return m;
}

/* Set n rectangles (x, y, w, h each) of mask m: those pixels get copied. */
static inline void x_mask_rects(unsigned int m, const short *r, int n)
{
    int most = (int)((sizeof xi_out - 12) / 8);

    xi_mask_fg(m, 1);
    while (n > 0) {
        int k = n < most ? n : most;

        xi_req(70, 0, 3 + (unsigned int)(k * 2));
        xi_put32(m);
        xi_put32(xi_gc1);
        for (int i = 0; i < k * 4; i++)
            xi_put16((unsigned int)r[i]);
        r += k * 4;
        n -= k;
    }
}

/* x_blit, copying only the pixels set in mask m (which lines up with c). */
static inline void x_blit_masked(unsigned int c, unsigned int m, int sx, int sy, int w, int h,
                                 int dx, int dy)
{
    if (w <= 0 || h <= 0)
        return;
    xi_req(56, 0, 3 + 3);               /* ChangeGC: clip origin and mask */
    xi_put32(xi_gc);
    xi_put32(0x20000 | 0x40000 | 0x80000);
    xi_put32((unsigned int)(dx - sx));
    xi_put32((unsigned int)(dy - sy));
    xi_put32(m);
    x_blit(c, sx, sy, w, h, dx, dy);
    xi_change_gc(0x80000, 0);           /* clip mask: None */
}

/*
 * A fence: x_fence marks the point reached, and x_fence_wait waits until
 * the server has dealt with everything up to the last mark. Waiting for
 * the previous frame's fence instead of x_sync keeps one frame in flight,
 * so the board draws the next frame while the last one travels.
 */
static inline void x_fence_wait(void)
{
    unsigned char b[32];

    x_flush();
    while (xi_fence_on && !xi_dead) {
        if (xi_message(b, NULL) < 0)
            break;
        xi_handle(b);
    }
    xi_fence_on = 0;
}

static inline void x_fence(void)
{
    x_fence_wait();                     /* only one at a time */
    xi_req(43, 0, 1);                   /* GetInputFocus: any request with a reply */
    xi_fence_seq = xi_seq & 0xffff;
    xi_fence_on = 1;
    x_flush();
}

/* Fill a rectangle in the current colour, but only where the mask m,
 * repeated across it like wallpaper, is set: a dither, for tints. */
static inline void x_fill_stippled(unsigned int m, int x, int y, int w, int h)
{
    xi_req(56, 0, 3 + 4);               /* ChangeGC: fill-style, stipple, ts origin */
    xi_put32(xi_gc);
    xi_put32(0x100 | 0x800 | 0x1000 | 0x2000);
    xi_put32(2);                        /* FillStippled */
    xi_put32(m);
    xi_put32(0);
    xi_put32(0);
    xi_box(70, x, y, w, h, 0);
    xi_change_gc(0x100, 0);             /* FillSolid */
}

/*
 * A stencil: a mask set up once for repeated masked copies. Setting a mask
 * makes the server turn the bitmap into a clip region, which is slow for
 * big masks; a stencil does that once, and each x_blit_stencil only moves
 * it. Free it with x_free_stencil (before freeing the mask).
 */
static inline unsigned int x_stencil(unsigned int m)
{
    unsigned int g = xi_new_id();

    xi_req(55, 0, 4 + 2);               /* CreateGC */
    xi_put32(g);
    xi_put32(xi_win);
    xi_put32(0x10000 | 0x80000);        /* graphics-exposures, clip-mask */
    xi_put32(0);
    xi_put32(m);
    return g;
}

static inline void x_free_stencil(unsigned int g)
{
    if (g == 0)
        return;
    xi_req(60, 0, 2);                   /* FreeGC */
    xi_put32(g);
}

/* x_blit through stencil g, made from the mask that lines up with c. */
static inline void x_blit_stencil(unsigned int g, unsigned int c, int sx, int sy, int w, int h,
                                  int dx, int dy)
{
    if (w <= 0 || h <= 0)
        return;
    xi_req(56, 0, 3 + 2);               /* ChangeGC: clip origin */
    xi_put32(g);
    xi_put32(0x20000 | 0x40000);
    xi_put32((unsigned int)(dx - sx));
    xi_put32((unsigned int)(dy - sy));
    xi_req(62, 0, 7);                   /* CopyArea */
    xi_put32(c);
    xi_put32(xi_dst());
    xi_put32(g);
    xi_put16((unsigned int)sx);
    xi_put16((unsigned int)sy);
    xi_put16((unsigned int)dx);
    xi_put16((unsigned int)dy);
    xi_put16((unsigned int)w);
    xi_put16((unsigned int)h);
}

/* Wait until the server has dealt with everything sent so far. */
static inline void x_sync(void)
{
    unsigned char b[32];

    xi_req(43, 0, 1);                   /* GetInputFocus: any request with a reply */
    xi_roundtrip(b, NULL);
}

/* Show what has been drawn since the last x_present(). */
static inline void x_present(void)
{
    xi_copy_back();
    x_flush();
}

/* The next event, if there is one; never blocks. */
static inline int x_event(struct x_event *e)
{
    unsigned char b[32];

    while (xi_qlen == 0 && !xi_dead) {
        struct pollfd p;

        p.fd = xi_fd;
        p.events = POLLIN;
        p.revents = 0;
        if (poll(&p, 1, 0) <= 0)
            break;
        if (xi_message(b, NULL) >= 0)
            xi_handle(b);
    }
    if (xi_qlen == 0)
        return 0;
    *e = xi_queue[xi_qhead];
    xi_qhead = (xi_qhead + 1) % (int)(sizeof xi_queue / sizeof xi_queue[0]);
    xi_qlen--;
    return 1;
}

/* Send what is buffered, then sleep until input arrives or ms pass. */
static inline void x_wait(int ms)
{
    struct pollfd p;

    x_flush();
    if (xi_qlen > 0 || xi_dead)
        return;
    p.fd = xi_fd;
    p.events = POLLIN;
    p.revents = 0;
    poll(&p, 1, ms);
}

/* Where the mouse is, relative to the window, even outside it. Returns 0
 * if it is on another screen or the server didn't answer. */
static inline int x_pointer(int *x, int *y)
{
    unsigned char b[32];

    xi_req(38, 0, 2);                   /* QueryPointer */
    xi_put32(xi_win);
    if (xi_roundtrip(b, NULL) < 0 || !b[1])
        return 0;
    *x = (short)xi_get16(b + 20);
    *y = (short)xi_get16(b + 22);
    return 1;
}

static inline int x_fd(void) { return xi_fd; }

#endif /* DEMO_X11_H */
