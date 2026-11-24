// SPDX-License-Identifier: MIT
/*
 * xeyes.c - a pair of eyes that follow the mouse, in an X11 window.
 *
 * Build:  cc -o xeyes xeyes.c
 * Run:    DISPLAY=HOST:0 ./xeyes     (see x11.h for letting the board in)
 * Keys:   q or Escape quits, as does closing the window.
 */
#include "x11.h"

static unsigned long long isqrt(unsigned long long v)
{
    unsigned long long r = 0, bit = 1ULL << 62;

    while (bit > v)
        bit >>= 2;
    while (bit != 0) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

static void oval(int cx, int cy, int rx, int ry)
{
    x_fill_ellipse(cx - rx, cy - ry, 2 * rx, 2 * ry);
}

/* One eye centred on (cx, cy), looking at the mouse at (mx, my). */
static void eye(int cx, int cy, int rx, int ry, int mx, int my)
{
    int m = rx < ry ? rx : ry;
    int t = m / 10 + 1;                 /* outline */
    int pr = m / 3;                     /* pupil */
    long long ax = rx - t - pr - 1, ay = ry - t - pr - 1;
    long long dx = mx - cx, dy = my - cy;
    unsigned long long lhs, lim;

    x_color(x_rgb(0, 0, 0));
    oval(cx, cy, rx, ry);
    x_color(x_rgb(255, 255, 255));
    oval(cx, cy, rx - t, ry - t);
    if (ax < 0)
        ax = 0;
    if (ay < 0)
        ay = 0;
    /* keep the pupil inside: (dx/ax)^2 + (dy/ay)^2 <= 1 */
    lhs = (unsigned long long)(dx * dx * ay * ay + dy * dy * ax * ax);
    lim = (unsigned long long)(ax * ax * ay * ay);
    if (lhs > lim) {
        long long s = (long long)isqrt(lim), d = (long long)isqrt(lhs);

        dx = d ? dx * s / d : 0;
        dy = d ? dy * s / d : 0;
    }
    x_color(x_rgb(0, 0, 0));
    oval(cx + (int)dx, cy + (int)dy, pr, pr);
}

int main(int argc, char **argv)
{
    int mx = 0, my = 0, last_x = -1, last_y = -1, dirty = 1;

    if (x_open(argc > 1 ? argv[1] : NULL) < 0)
        return 1;
    x_window("xeyes", 320, 200);
    for (;;) {
        struct x_event e;
        int rx = x_width / 4 - 4, ry = x_height / 2 - 4;

        while (x_event(&e)) {
            if (e.type == XE_CLOSE)
                return 0;
            if (e.type == XE_KEY_DOWN && (e.key == 'q' || e.key == X_KEY_ESCAPE)) {
                x_close();
                return 0;
            }
            if (e.type == XE_RESIZE)
                dirty = 1;
        }
        if (x_pointer(&mx, &my) && (mx != last_x || my != last_y)) {
            last_x = mx;
            last_y = my;
            dirty = 1;
        }
        if (dirty) {
            x_color(x_rgb(255, 255, 255));
            x_fill_rect(0, 0, x_width, x_height);
            if (rx > 4 && ry > 4) {
                eye(x_width / 4, x_height / 2, rx, ry, mx, my);
                eye(3 * x_width / 4, x_height / 2, rx, ry, mx, my);
            }
            x_present();
            dirty = 0;
        }
        x_wait(30);
    }
}
