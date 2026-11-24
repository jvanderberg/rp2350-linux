// SPDX-License-Identifier: MIT
/*
 * xterm.c - a terminal in an X11 window: a shell (or a command) on a
 * pseudo-terminal, shown through tm's terminal emulator (vt.c).
 *
 * Build:  ./build.sh xterm      (xterm.c with vt.c, sb.c, lz.c, util.c;
 *                                needs ../demos/x11.h)
 * Run:    DISPLAY=HOST:0 ./xterm [-fs PIXELS] [-g COLSxROWS] [-T TITLE]
 *                                [-hold] [-e COMMAND [ARGS...]]
 *
 * Programs see TERM=screen-256color, which is what vt.c implements: vi,
 * less, top, mc and tm all work. The wheel scrolls back through history
 * (Shift-PgUp/PgDn too) unless the program asked for the mouse, as mc
 * does; then it gets the clicks. The window can be resized.
 *
 * The font is the first of misc-fixed (the classic xterm font, which has
 * the line-drawing characters), DejaVu Sans Mono and Lucida Typewriter
 * that the X server has, at -fs pixels: 13 by default; misc-fixed also
 * comes in 14, 15, 18 and 20.
 *
 * The terminal itself is term.h, which xmc uses for its output view too.
 */
#include "x11.h"

#include "term.h"
#include "util.h"

#define PAD 6

int main(int argc, char **argv)
{
    static struct term t;
    int px = 13, cols = 100, rows = 32, hold = 0, i, last_w, last_h;
    long long resize_at = 0;
    const char *title = "xterm";
    char **cmd = NULL;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            cmd = argv + i + 1;
            if (strcmp(title, "xterm") == 0)
                title = argv[i + 1];
            break;
        } else if (strcmp(argv[i], "-fs") == 0 && i + 1 < argc) {
            px = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "-g") == 0 || strcmp(argv[i], "-geometry") == 0) &&
                   i + 1 < argc) {
            sscanf(argv[++i], "%dx%d", &cols, &rows);
        } else if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
            title = argv[++i];
        } else if (strcmp(argv[i], "-hold") == 0) {
            hold = 1;
        } else {
            fprintf(stderr, "usage: xterm [-fs PIXELS] [-g COLSxROWS] [-T TITLE] [-hold]"
                            " [-e COMMAND [ARGS...]]\n");
            return 2;
        }
    }
    if (px < 6 || px > 48)
        px = 13;
    if (cols < 10 || cols > 400)
        cols = 100;
    if (rows < 3 || rows > 200)
        rows = 32;
    signal(SIGPIPE, SIG_IGN);

    if (x_open(NULL) < 0)
        return 1;
    term_init(&t, px);
    t.focus = 1;
    x_background(t.bg);
    x_window(title, cols * t.cw + 2 * PAD, rows * t.ch + 2 * PAD);
    x_min_size(20 * t.cw + 2 * PAD, 4 * t.ch + 2 * PAD);
    x_color(t.bg);
    x_fill_rect(0, 0, x_width, x_height);
    term_place(&t, PAD, PAD, x_width - 2 * PAD, x_height - 2 * PAD);
    if (term_start(&t, NULL, cmd) != 0) {
        perror("xterm: pseudo-terminal");
        return 1;
    }
    last_w = x_width;
    last_h = x_height;
    term_draw(&t);
    x_present();

    for (;;) {
        struct x_event e;
        struct pollfd p[2];
        int dirty = 0, n = 0;

        while (x_event(&e) && n++ < 256) {
            switch (e.type) {
            case XE_CLOSE:
                term_close(&t);
                x_close();
                return 0;
            case XE_RESIZE:
                resize_at = now_ms() + 80;      /* when it settles */
                break;
            case XE_KEY_DOWN:
                if (!t.running && hold && !(e.key >= X_KEY_SHIFT_L && e.key <= X_KEY_CONTROL_R))
                    goto out;
                dirty |= term_key(&t, &e);
                break;
            case XE_BUTTON_DOWN: case XE_BUTTON_UP: case XE_MOTION:
                dirty |= term_mouse(&t, &e);
                break;
            }
        }
        if (resize_at && now_ms() >= resize_at && (x_width != last_w || x_height != last_h)) {
            resize_at = 0;
            last_w = x_width;
            last_h = x_height;
            x_color(t.bg);
            x_fill_rect(0, 0, x_width, x_height);
            term_place(&t, PAD, PAD, x_width - 2 * PAD, x_height - 2 * PAD);
            dirty = 1;
        }
        if (t.running && term_pump(&t)) {
            dirty = 1;
            if (!t.running) {
                if (!hold)
                    break;
                vt_write(&t.vt, (const unsigned char *)"\r\n[finished: press a key to close]", 35);
            }
        }
        if (t.vt.dirty || dirty) {
            t.vt.dirty = 0;
            x_fence_wait();
            term_draw(&t);
            x_present();
            x_fence();
        }
        x_flush();
        if (x_pending())
            continue;
        p[0].fd = x_fd();
        p[0].events = POLLIN;
        p[1].fd = term_fd(&t);
        p[1].events = POLLIN;
        poll(p, 2, resize_at ? 80 : -1);
    }
out:
    term_close(&t);
    x_close();
    return 0;
}
