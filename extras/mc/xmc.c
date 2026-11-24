// SPDX-License-Identifier: MIT
/*
 * xmc.c - the mc file manager in an X11 window, in xpulse's style.
 *
 * Build:  ./build.sh xmc        (xmc.c with mc's panel.c, fileops.c,
 *                                usermenu.c and util.c; needs ../demos/x11.h)
 * Run:    DISPLAY=HOST:0 ./xmc [left-dir [right-dir]]
 *
 * mc's logic is shared: reading and sorting directories, marks, copy, move,
 * delete and mkdir with their questions and progress, and the user menu.
 * The dialogs those ask through (dialog.h) are drawn here, in the window.
 *
 * Mouse:  click a file to select it, double-click to open it; Ctrl-click
 *         marks one, Shift-click marks a range, Shift- or Ctrl-drag marks
 *         the rows passed over. Drag files to the other panel to copy them
 *         (Shift: move), onto a folder to put them in it, or onto the
 *         command line to type their names. Right-click for a menu of what
 *         can be done (the user menu's entries are at the bottom). Click a
 *         part of a path to go there, a column heading to sort by it.
 * Keys:   as in mc: arrows, Tab, Enter, Insert, F3 view, F4 edit, F5 copy,
 *         F6 move, F7 mkdir, F8 delete, F2 menu, F10 quit, C-o the latest
 *         terminal tab, C-r rescan, C-u swap, C-n a shell, M-. dot files;
 *         Shift-arrows mark; Ctrl-Tab the next tab. Anything typed goes to
 *         the command line.
 *
 * Tabs: the bar along the top holds files tabs (+ opens another, where
 * this one is; each goes its own way) and terminal tabs. A command typed
 * on the command line, a program opened with a double-click, a user menu
 * command, F4 (the editor) and C-n (a shell) each get a terminal tab of
 * their own (../tm/term.h), where the keys go to the program while it
 * runs. A tab is shown when its program first writes something, soon
 * after it starts; one that ends well with nothing on its screen (an X
 * program, a command with nothing to say, vi when you quit) closes by
 * itself. Once its program has ended, a key closes the tab. A tab's close
 * button, or the middle button, closes it; a program still running in
 * it is hung up on. The gear holds the settings.
 *
 * Like xpulse, nothing is drawn twice: each row, title and line is sent
 * only when what it shows changed, lists scroll with one copy on the
 * server, and menus and dialogs put back what they covered with one copy.
 */
#include "x11.h"
#include "../tm/term.h"
#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <strings.h>
#include <time.h>

#include "dialog.h"
#include "fileops.h"
#include "keys.h"
#include "panel.h"
#include "run.h"
#include "screen.h"
#include "skin.h"
#include "usermenu.h"
#include "util.h"

#define M 12                    /* window margin */
#define G 10                    /* gap between panels */
#define P 12                    /* padding inside a panel */
#define RADIUS 7
#define MAXROWS 96              /* rows of a list on screen, at most */
#define KEYLEN 320
#define SETTLE_MS 150           /* a resize is done when it stops this long */
#define DCLICK_MS 400
#define DRAG_PX 5

/* ====================================================================
 * style: xpulse's colours and fonts
 * ==================================================================== */

typedef struct { int r, g, b; } rgb;
typedef struct { int x, y, w, h; } box;

static const rgb RGB_BG = { 13, 17, 23 }, RGB_PANEL = { 22, 27, 34 };
static const rgb RGB_TEXT = { 230, 237, 243 };
static const rgb ACC_BLUE = { 88, 166, 255 }, ACC_GREEN = { 63, 185, 80 };
static const rgb ACC_AMBER = { 227, 179, 65 }, ACC_RED = { 248, 81, 73 };
static const rgb ACC_MARK = { 255, 210, 90 };

static unsigned c_bg, c_panel, c_grid, c_track, c_text, c_dim, c_faint, c_sel, c_sel_dim,
    c_mark_bg, c_popup;
static int f_head, f_bold, f_text, f_mono;
static unsigned type_px[16];     /* mc's 16 colours, for file types */

static unsigned px(rgb c) { return x_rgb(c.r, c.g, c.b); }

static rgb mix(rgb a, rgb b, int t)     /* a + (b - a) * t / 1000 */
{
    rgb c = { a.r + (b.r - a.r) * t / 1000, a.g + (b.g - a.g) * t / 1000,
              a.b + (b.b - a.b) * t / 1000 };
    return c;
}

static int font_h(int f) { return x_font_ascent(f) + x_font_descent(f); }

static void setup_style(void)
{
    /* mc's file type colours, as they look on xpulse's dark panels */
    static const rgb types[16] = {
        { 110, 118, 129 }, { 248, 81, 73 },   { 86, 211, 100 },  { 227, 179, 65 },
        { 88, 166, 255 },  { 188, 140, 255 }, { 57, 197, 207 },  { 201, 209, 217 },
        { 110, 118, 129 }, { 255, 123, 114 }, { 126, 231, 135 }, { 242, 204, 96 },
        { 121, 192, 255 }, { 210, 168, 255 }, { 86, 212, 221 },  { 230, 237, 243 },
    };
    rgb dim = { 139, 148, 158 }, faint = { 72, 79, 88 };

    c_bg = px(RGB_BG);
    c_panel = px(RGB_PANEL);
    c_grid = px(mix(RGB_PANEL, RGB_TEXT, 60));
    c_track = px(mix(RGB_PANEL, RGB_TEXT, 90));
    c_text = px(RGB_TEXT);
    c_dim = px(dim);
    c_faint = px(faint);
    c_sel = px(mix(RGB_PANEL, ACC_BLUE, 220));
    c_sel_dim = px(mix(RGB_PANEL, RGB_TEXT, 55));
    c_mark_bg = px(mix(RGB_PANEL, ACC_MARK, 90));
    c_popup = px(mix(RGB_PANEL, RGB_BG, 400));
    for (int i = 0; i < 16; i++)
        type_px[i] = px(types[i]);

    f_head = x_font("-adobe-helvetica-bold-r-normal--17-*-*-*-p-*-iso8859-1");
    f_bold = x_font("-adobe-helvetica-bold-r-normal--12-*-*-*-p-*-iso8859-1");
    f_text = x_font("-adobe-helvetica-medium-r-normal--12-*-*-*-p-*-iso8859-1");
    f_mono = x_font("-b&h-lucidatypewriter-medium-r-normal-sans-12-*-*-*-m-*-iso8859-1");
}

static void round_rect(int x, int y, int w, int h, int r)
{
    if (w <= 0 || h <= 0)
        return;
    if (r < 1 || w < 2 * r || h < 2 * r) {
        x_fill_rect(x, y, w, h);
        return;
    }
    x_fill_rect(x + r, y, w - 2 * r, h);
    x_fill_rect(x, y + r, w, h - 2 * r);
    x_fill_ellipse(x, y, 2 * r, 2 * r);
    x_fill_ellipse(x + w - 2 * r, y, 2 * r, 2 * r);
    x_fill_ellipse(x, y + h - 2 * r, 2 * r, 2 * r);
    x_fill_ellipse(x + w - 2 * r, y + h - 2 * r, 2 * r, 2 * r);
}

static void triangle(int x, int y, int down, unsigned color)
{
    short t[6] = { (short)x, (short)(down ? y : y + 4), (short)(x + 8), (short)(down ? y : y + 4),
                   (short)(x + 4), (short)(down ? y + 4 : y) };

    x_color(color);
    x_fill_poly(t, 3);
}

/* ---- text ---- */

/* The fonts are ISO 8859-1: file names come as UTF-8, and anything past
 * Latin-1 shows as '?'. */
static void to_latin1(char *out, size_t cap, const char *s)
{
    size_t n = 0;

    while (*s && n + 1 < cap) {
        unsigned c = utf8_next(&s);

        out[n++] = (char)(c < 256 && c >= 32 ? c : c == '\t' ? ' ' : '?');
    }
    out[n] = 0;
}

enum { TRIM_END, TRIM_MID, TRIM_START };

/* s (Latin-1) shortened with "..." to fit w pixels in font f. */
static void fit(char *out, size_t cap, int f, const char *s, int w, int how)
{
    int n = (int)strlen(s), dots = x_text_width(f, "...");

    snprintf(out, cap, "%s", s);
    if (x_text_width(f, out) <= w)
        return;
    if (w <= dots) {
        out[0] = 0;
        return;
    }
    if (how == TRIM_START) {
        for (int k = 1; k < n; k++) {
            snprintf(out, cap, "...%s", s + k);
            if (x_text_width(f, out) <= w)
                return;
        }
    } else if (how == TRIM_MID) {
        for (int cut = 1; cut < n; cut++) {
            int keep = n - cut, head = (keep + 1) / 2, tail = keep - head;

            snprintf(out, cap, "%.*s...%s", head, s, s + n - tail);
            if (x_text_width(f, out) <= w)
                return;
        }
    } else {
        for (int k = n - 1; k > 0; k--) {
            snprintf(out, cap, "%.*s...", k, s);
            if (x_text_width(f, out) <= w)
                return;
        }
    }
    out[0] = 0;
}

/* Text with its top at y in [x, x + w), shortened to fit. */
static void text_at(int f, int x, int y, int w, int align, int how, unsigned color, const char *s)
{
    char buf[512];
    int tw;

    fit(buf, sizeof buf, f, s, w, how);
    if (!buf[0])
        return;
    tw = x_text_width(f, buf);
    x_text_font(f);
    x_color(color);
    x_text(align == 1 ? x + w - tw : align == 2 ? x + (w - tw) / 2 : x, y + x_font_ascent(f), buf);
}

/* ====================================================================
 * overlays: what a menu, dialog or drag label covers is saved on the
 * server and put back with one copy when it goes
 * ==================================================================== */

typedef struct { box b; unsigned save; } under;

static void under_save(under *u, box b)
{
    if (b.x < 0) { b.w += b.x; b.x = 0; }
    if (b.y < 0) { b.h += b.y; b.y = 0; }
    if (b.x + b.w > x_width) b.w = x_width - b.x;
    if (b.y + b.h > x_height) b.h = x_height - b.y;
    u->b = b;
    u->save = 0;
    if (b.w <= 0 || b.h <= 0)
        return;
    u->save = x_canvas(b.w, b.h);
    x_target(u->save);
    x_blit(x_frame(), b.x, b.y, b.w, b.h, 0, 0);
    x_target(0);
}

static void under_restore(under *u)
{
    if (!u->save)
        return;
    x_blit(u->save, 0, 0, u->b.w, u->b.h, u->b.x, u->b.y);
    x_free_canvas(u->save);
    u->save = 0;
}

static int overlays;            /* menus and dialogs open: the view is frozen */
static int resize_pending;
static long long resize_at;

/* ====================================================================
 * input: X events as mc's key codes (keys.h)
 * ==================================================================== */

static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int xkey(const struct x_event *e)
{
    int k = e->key, m = e->mods, r;

    switch (k) {
    case X_KEY_UP: r = K_UP; break;
    case X_KEY_DOWN: r = K_DOWN; break;
    case X_KEY_LEFT: r = K_LEFT; break;
    case X_KEY_RIGHT: r = K_RIGHT; break;
    case X_KEY_HOME: r = K_HOME; break;
    case X_KEY_END: r = K_END; break;
    case X_KEY_PAGE_UP: r = m & X_MOD_CTRL ? K_CTRL_PGUP : K_PGUP; break;
    case X_KEY_PAGE_DOWN: r = m & X_MOD_CTRL ? K_CTRL_PGDN : K_PGDN; break;
    case X_KEY_INSERT: r = K_INS; break;
    case X_KEY_DELETE: r = K_DEL; break;
    case X_KEY_RETURN: case X_KEY_KP_ENTER: r = K_ENTER; break;
    case X_KEY_TAB: r = m & X_MOD_SHIFT ? K_BTAB : K_TAB; break;
    case X_KEY_BACKTAB: r = K_BTAB; break;
    case X_KEY_ESCAPE: r = K_ESC; break;
    case X_KEY_BACKSPACE: r = K_BS; break;
    default:
        if (k >= X_KEY_F1 && k < X_KEY_F1 + 12)
            r = K_F(k - X_KEY_F1 + 1);
        else if (k >= 32 && k < 256)
            r = (m & X_MOD_CTRL) && isalpha(k) ? K_CTRL(k) : k;
        else
            return K_NONE;              /* a modifier on its own */
    }
    if (m & X_MOD_ALT)
        r |= K_ALT;
    return r;
}

/* The command running, if any: its output is read between events. */
static void job_poll(int ms);

/* Wait up to ms (-1: as long as it takes) for an X event. */
static int wake_on_output;        /* return from next_event when output arrives */
static int job_dirty;

static int next_event(struct x_event *e, int ms)
{
    long long until = ms < 0 ? -1 : now_ms() + ms;

    for (;;) {
        if (x_event(e)) {
            if (e->type == XE_RESIZE) {
                resize_pending = 1;     /* laid out when the size settles */
                resize_at = now_ms() + SETTLE_MS;
                continue;
            }
            return 1;
        }
        if (until >= 0 && now_ms() >= until)
            return 0;
        job_poll(until < 0 ? 100 : (int)(until - now_ms()));
        if (wake_on_output && job_dirty)
            return 0;
    }
}

/* ====================================================================
 * dialogs: dialog.h, drawn in the window
 * ==================================================================== */

struct wgeo { int x, y, w, h; };
static struct dlgview {
    struct dialog *d;
    box b;
    under u;
    struct wgeo g[DLG_MAX_WIDGETS];
    char key[DLG_MAX_WIDGETS][KEYLEN];
    int hover;
} dstack[8];
static int ndlg;

struct mouse_event key_mouse;
int scr_rows = 40, scr_cols = 120;      /* in nominal cells, for dialog sizes */
volatile sig_atomic_t scr_winch, scr_quit;

int scr_update_size(void) { return 0; }

void dlg_init(struct dialog *d, const char *title, int h, int w, int scheme)
{
    memset(d, 0, sizeof *d);
    d->title = title;
    d->h = h;
    d->w = w;
    d->scheme = scheme;
    d->focus = -1;
}

static struct widget *dlg_add(struct dialog *d, int type, int y)
{
    struct widget *w;

    if (d->n >= DLG_MAX_WIDGETS)
        d->n = DLG_MAX_WIDGETS - 1;     /* reuse the last; never happens */
    w = &d->wd[d->n++];
    memset(w, 0, sizeof *w);
    w->type = type;
    w->y = y;
    return w;
}

struct widget *dlg_label(struct dialog *d, int y, const char *text, int center)
{
    struct widget *w = dlg_add(d, W_LABEL, y);

    w->text = text;
    w->flags = center ? WF_CENTER : 0;
    return w;
}

struct widget *dlg_input(struct dialog *d, int y, char *buf, int cap)
{
    struct widget *w = dlg_add(d, W_INPUT, y);

    w->buf = buf;
    w->cap = cap;
    w->pos = (int)strlen(buf);
    return w;
}

struct widget *dlg_gauge(struct dialog *d, int y)
{
    return dlg_add(d, W_GAUGE, y);
}

struct widget *dlg_list_box(struct dialog *d, int y, int h, const char *const *items,
                            const char *hotkeys, int n)
{
    struct widget *w = dlg_add(d, W_LIST, y);

    w->items = items;
    w->hotkeys = hotkeys;
    w->cap = n;
    w->h = h;
    w->id = 0;
    return w;
}

void dlg_hline(struct dialog *d, int y)
{
    dlg_add(d, W_HLINE, y);
}

void dlg_buttons(struct dialog *d, int y, int gap, const char *const *labels, int n, int def)
{
    (void)gap;
    for (int i = 0; i < n; i++) {
        struct widget *w = dlg_add(d, W_BUTTON, y);

        w->text = labels[i];
        w->id = i;
        w->flags = i == def ? WF_DEFAULT : 0;
    }
}

static int focusable(const struct widget *w)
{
    return w->type == W_INPUT || w->type == W_BUTTON || w->type == W_LIST;
}

/* A button's caption without its '&', and the hotkey it marked. */
static int caption(const char *s, char *out, size_t cap, int *at)
{
    size_t n = 0;
    int hot = 0;

    *at = -1;
    for (; *s && n + 1 < cap; s++) {
        if (*s == '&' && s[1]) {
            hot = tolower((unsigned char)s[1]);
            *at = (int)n;
            continue;
        }
        out[n++] = *s;
    }
    out[n] = 0;
    return hot;
}

#define ROW_ITEM 22

/* Lay the dialog out in pixels: rows in the order of their y, buttons
 * centred side by side. */
static void dlg_layout(struct dlgview *v)
{
    struct dialog *d = v->d;
    int w = d->w * 8 + 32, y = 46, maxy = 0;

    if (w < 360)
        w = 360;
    if (w > x_width - 40)
        w = x_width - 40;
    for (int i = 0; i < d->n; i++)
        if (d->wd[i].y > maxy)
            maxy = d->wd[i].y;
    for (int row = 0; row <= maxy; row++) {
        int rh = 0, nb = 0, bw = 0, bx;

        for (int i = 0; i < d->n; i++) {
            struct widget *wd = &d->wd[i];
            int h = 0;
            char cap[64];
            int at;

            if (wd->y != row)
                continue;
            switch (wd->type) {
            case W_LABEL: h = font_h(f_text) + 4; break;
            case W_INPUT: h = 30; break;
            case W_GAUGE: h = 16; break;
            case W_HLINE: h = 16; break;
            case W_BUTTON:
                h = 30;
                caption(wd->text, cap, sizeof cap, &at);
                v->g[i].w = x_text_width(f_bold, cap) + 32;
                bw += v->g[i].w + (nb ? 8 : 0);
                nb++;
                break;
            case W_LIST: h = wd->h * ROW_ITEM + 4; break;
            }
            if (wd->type != W_BUTTON) {
                v->g[i].x = 18;
                v->g[i].w = w - 36;
            }
            v->g[i].y = y;
            v->g[i].h = h;
            if (h > rh)
                rh = h;
        }
        bx = (w - bw) / 2;
        for (int i = 0; i < d->n; i++)
            if (d->wd[i].y == row && d->wd[i].type == W_BUTTON) {
                v->g[i].x = bx;
                bx += v->g[i].w + 8;
            }
        y += rh ? rh + 4 : (row >= 2 ? 4 : 0);
    }
    v->b.w = w;
    v->b.h = y + 14;
    if (v->b.h > x_height - 20)
        v->b.h = x_height - 20;
    v->b.x = (x_width - v->b.w) / 2;
    v->b.y = (x_height - v->b.h) / 3;
    if (v->b.y < 10)
        v->b.y = 10;
}

static rgb dlg_accent(const struct dialog *d)
{
    return d->scheme == DLG_ERROR ? ACC_RED : d->scheme == DLG_MENU ? ACC_GREEN : ACC_BLUE;
}

static void dlg_frame(struct dlgview *v)
{
    box b = v->b;
    char t[256];

    x_color(px(mix(RGB_BG, RGB_PANEL, 300)));
    round_rect(b.x - 4, b.y - 2, b.w + 8, b.h + 8, 10);   /* a soft shadow */
    x_color(c_track);
    round_rect(b.x - 1, b.y - 1, b.w + 2, b.h + 2, 9);
    x_color(c_popup);
    round_rect(b.x, b.y, b.w, b.h, 8);
    x_color(px(dlg_accent(v->d)));
    round_rect(b.x + 18, b.y + 17, 4, x_font_ascent(f_bold) - 1, 2);
    to_latin1(t, sizeof t, v->d->title ? v->d->title : "");
    text_at(f_bold, b.x + 28, b.y + 14, b.w - 46, 0, TRIM_END, c_text, t);
    memset(v->key, 0, sizeof v->key);
}

/* Draw the widgets whose look changed. */
static void dlg_widgets(struct dlgview *v)
{
    struct dialog *d = v->d;

    for (int i = 0; i < d->n; i++) {
        struct widget *w = &d->wd[i];
        struct wgeo g = v->g[i];
        int x = v->b.x + g.x, y = v->b.y + g.y, focus = d->focus == i;
        char key[KEYLEN], t[512];

        if (g.y + g.h > v->b.h)
            continue;                   /* doesn't fit: left out */
        switch (w->type) {
        case W_LABEL:
            snprintf(key, sizeof key, "L%s", w->text ? w->text : "");
            break;
        case W_INPUT:
            snprintf(key, sizeof key, "I%d|%d|%d|%s", focus, w->pos, w->changed, w->buf);
            break;
        case W_GAUGE:
            snprintf(key, sizeof key, "G%d", g.w * w->value / 1000);
            break;
        case W_BUTTON:
            snprintf(key, sizeof key, "B%d|%d", focus, v->hover == i);
            break;
        case W_LIST:
            snprintf(key, sizeof key, "S%d|%d|%d|%d", focus, w->pos, w->scroll, v->hover);
            break;
        default:
            snprintf(key, sizeof key, "H");
        }
        if (strcmp(key, v->key[i]) == 0)
            continue;
        strcpy(v->key[i], key);
        x_color(c_popup);
        x_fill_rect(x, y, g.w, g.h);
        switch (w->type) {
        case W_LABEL:
            to_latin1(t, sizeof t, w->text ? w->text : "");
            text_at(f_text, x, y + 1, g.w, w->flags & WF_CENTER ? 2 : 0, TRIM_START, c_text, t);
            break;
        case W_HLINE:
            x_color(c_grid);
            x_fill_rect(x, y + g.h / 2, g.w, 1);
            break;
        case W_GAUGE:
            x_color(c_track);
            round_rect(x, y + 4, g.w, 8, 4);
            if (w->value > 0) {
                int f = (int)((long)g.w * (w->value > 1000 ? 1000 : w->value) / 1000);

                x_color(px(dlg_accent(d)));
                round_rect(x, y + 4, f < 8 ? 8 : f, 8, 4);
            }
            break;
        case W_INPUT: {
            int ty = y + (g.h - font_h(f_text)) / 2, room = g.w - 20, cx, start = 0;
            char shown[512];

            x_color(focus ? px(ACC_BLUE) : c_track);
            round_rect(x, y, g.w, g.h, 6);
            x_color(c_bg);
            round_rect(x + 1, y + 1, g.w - 2, g.h - 2, 5);
            to_latin1(t, sizeof t, w->buf);
            /* scroll so the caret is in view */
            {
                char before[512], head[512];

                snprintf(head, sizeof head, "%.*s", w->pos, w->buf);
                to_latin1(before, sizeof before, head);
                while (x_text_width(f_text, before + start) > room && before[start])
                    start++;
                snprintf(shown, sizeof shown, "%s", t + start);
                cx = x + 10 + x_text_width(f_text, before + start);
            }
            if (!w->changed && w->buf[0] && focus) {   /* typing replaces it all */
                x_color(c_sel);
                x_fill_rect(x + 8, ty - 1, x_text_width(f_text, shown) + 4, font_h(f_text) + 2);
            }
            text_at(f_text, x + 10, ty, room, 0, TRIM_END, c_text, shown);
            if (focus) {
                x_color(px(ACC_BLUE));
                x_fill_rect(cx, ty, 1, font_h(f_text));
            }
            break;
        }
        case W_BUTTON: {
            int at, hot;
            char cap[64];
            unsigned bg = w->flags & WF_DEFAULT ? px(mix(RGB_PANEL, dlg_accent(d), 450))
                                                : c_track;

            hot = caption(w->text, cap, sizeof cap, &at);
            (void)hot;
            if (focus || v->hover == i) {
                x_color(focus ? px(dlg_accent(d)) : c_dim);
                round_rect(x - 2, y - 2, g.w + 4, g.h + 4, 8);
            }
            x_color(v->hover == i ? px(mix(RGB_PANEL, dlg_accent(d), 600)) : bg);
            round_rect(x, y, g.w, g.h, 6);
            text_at(f_bold, x, y + (g.h - font_h(f_bold)) / 2, g.w, 2, TRIM_END, c_text, cap);
            if (at >= 0) {              /* underline the hotkey */
                char pre[64];
                int tw = x_text_width(f_bold, cap), lx;

                snprintf(pre, sizeof pre, "%.*s", at, cap);
                lx = x + (g.w - tw) / 2 + x_text_width(f_bold, pre);
                snprintf(pre, sizeof pre, "%c", cap[at]);
                x_color(c_text);
                x_fill_rect(lx, y + (g.h - font_h(f_bold)) / 2 + x_font_ascent(f_bold) + 2,
                            x_text_width(f_bold, pre), 1);
            }
            break;
        }
        case W_LIST:
            if (w->pos < w->scroll)
                w->scroll = w->pos;
            if (w->pos >= w->scroll + w->h)
                w->scroll = w->pos - w->h + 1;
            for (int r = 0; r < w->h && w->scroll + r < w->cap; r++) {
                int k = w->scroll + r, ry = y + 2 + r * ROW_ITEM;

                if (k == w->pos || v->hover == 100 + k) {
                    x_color(k == w->pos ? c_sel : c_sel_dim);
                    round_rect(x, ry, g.w, ROW_ITEM, 4);
                }
                to_latin1(t, sizeof t, w->items[k]);
                text_at(f_text, x + 10, ry + (ROW_ITEM - font_h(f_text)) / 2, g.w - 20, 0,
                        TRIM_END, c_text, t);
            }
            break;
        }
    }
}

static void dlg_push(struct dialog *d)
{
    struct dlgview *v;

    if (ndlg >= (int)(sizeof dstack / sizeof dstack[0]))
        return;
    v = &dstack[ndlg++];
    memset(v, 0, sizeof *v);
    v->d = d;
    v->hover = -1;
    if (d->focus < 0) {
        for (int i = 0; i < d->n && d->focus < 0; i++)
            if (d->wd[i].type == W_INPUT || d->wd[i].type == W_LIST)
                d->focus = i;
        for (int i = 0; i < d->n && d->focus < 0; i++)
            if (d->wd[i].type == W_BUTTON && (d->wd[i].flags & WF_DEFAULT))
                d->focus = i;
        for (int i = 0; i < d->n && d->focus < 0; i++)
            if (focusable(&d->wd[i]))
                d->focus = i;
    }
    dlg_layout(v);
    overlays++;
    under_save(&v->u, (box){ v->b.x - 6, v->b.y - 4, v->b.w + 12, v->b.h + 12 });
    dlg_frame(v);
    dlg_widgets(v);
}

static void dlg_pop(void)
{
    if (ndlg == 0)
        return;
    ndlg--;
    under_restore(&dstack[ndlg].u);
    overlays--;
    x_present();
    x_flush();
}

void dlg_show(struct dialog *d)
{
    dlg_push(d);
    x_present();
    x_sync();
}

void dlg_hide(struct dialog *d)
{
    (void)d;
    dlg_pop();
}

void dlg_paint(void)
{
    for (int i = 0; i < ndlg; i++)
        dlg_widgets(&dstack[i]);
}

void dlg_redraw(void)
{
    dlg_paint();
    x_present();
    x_sync();
}

static int dlg_hit(struct dlgview *v, int x, int y)
{
    for (int i = 0; i < v->d->n; i++) {
        struct wgeo g = v->g[i];

        if (x >= v->b.x + g.x && x < v->b.x + g.x + g.w && y >= v->b.y + g.y &&
            y < v->b.y + g.y + g.h)
            return i;
    }
    return -1;
}

static int dlg_default(struct dialog *d)
{
    for (int i = 0; i < d->n; i++)
        if (d->wd[i].type == W_BUTTON && (d->wd[i].flags & WF_DEFAULT))
            return d->wd[i].id;
    for (int i = 0; i < d->n; i++)
        if (d->wd[i].type == W_BUTTON)
            return d->wd[i].id;
    return 0;
}

/* Editing keys for an input field. */
static void input_key(struct widget *w, int k)
{
    int len = (int)strlen(w->buf);

    if (k == K_LEFT || k == K_RIGHT || k == K_HOME || k == K_END)
        w->changed = 1;
    switch (k) {
    case K_LEFT:
        while (w->pos > 0 && ((unsigned char)w->buf[--w->pos] & 0xC0) == 0x80)
            ;
        return;
    case K_RIGHT:
        while (w->pos < len && ((unsigned char)w->buf[++w->pos] & 0xC0) == 0x80)
            ;
        return;
    case K_HOME: case K_CTRL('a'):
        w->pos = 0;
        return;
    case K_END: case K_CTRL('e'):
        w->pos = len;
        return;
    case K_BS:
        if (!w->changed) {
            w->buf[0] = 0;
            w->pos = 0;
        } else if (w->pos > 0) {
            int p = w->pos;

            while (p > 0 && ((unsigned char)w->buf[--p] & 0xC0) == 0x80)
                ;
            memmove(w->buf + p, w->buf + w->pos, (size_t)(len - w->pos + 1));
            w->pos = p;
        }
        w->changed = 1;
        return;
    case K_DEL: {
        int p = w->pos;

        if (p >= len)
            return;
        while (++p < len && ((unsigned char)w->buf[p] & 0xC0) == 0x80)
            ;
        memmove(w->buf + w->pos, w->buf + p, (size_t)(len - p + 1));
        w->changed = 1;
        return;
    }
    case K_CTRL('u'):
        w->buf[0] = 0;
        w->pos = 0;
        w->changed = 1;
        return;
    }
    if (k >= 32 && k < 256 && k != 127) {
        char u[3];
        int n = 1;

        if (!w->changed) {
            w->buf[0] = 0;
            w->pos = len = 0;
        }
        w->changed = 1;
        if (k < 128) {
            u[0] = (char)k;
        } else {                        /* Latin-1 key: as UTF-8 */
            u[0] = (char)(0xC0 | (k >> 6));
            u[1] = (char)(0x80 | (k & 0x3F));
            n = 2;
        }
        if (len + n < w->cap) {
            memmove(w->buf + w->pos + n, w->buf + w->pos, (size_t)(len - w->pos + 1));
            memcpy(w->buf + w->pos, u, (size_t)n);
            w->pos += n;
        }
    }
}

static void focus_step(struct dialog *d, int dir)
{
    for (int k = 0; k < d->n; k++) {
        d->focus = (d->focus + dir + d->n) % d->n;
        if (focusable(&d->wd[d->focus]))
            return;
    }
}

int dlg_run(struct dialog *d)
{
    struct dlgview *v;
    int result = -2;

    dlg_push(d);
    v = &dstack[ndlg - 1];
    x_present();
    x_sync();
    while (result == -2) {
        struct x_event e;
        struct widget *fw;
        int k, i;

        if (!next_event(&e, -1))
            continue;
        fw = d->focus >= 0 ? &d->wd[d->focus] : NULL;
        switch (e.type) {
        case XE_CLOSE:
            result = -1;
            break;
        case XE_MOTION:
            i = dlg_hit(v, e.x, e.y);
            if (i >= 0 && d->wd[i].type == W_LIST) {
                int r = (e.y - (v->b.y + v->g[i].y + 2)) / ROW_ITEM + d->wd[i].scroll;

                i = r >= 0 && r < d->wd[i].cap ? 100 + r : -1;
            } else if (i >= 0 && d->wd[i].type != W_BUTTON) {
                i = -1;
            }
            v->hover = i;
            break;
        case XE_BUTTON_DOWN:
            i = dlg_hit(v, e.x, e.y);
            if (i < 0)
                break;
            if (e.button == 4 || e.button == 5) {
                if (d->wd[i].type == W_LIST) {
                    struct widget *w = &d->wd[i];

                    w->pos += e.button == 4 ? -1 : 1;
                    if (w->pos < 0) w->pos = 0;
                    if (w->pos >= w->cap) w->pos = w->cap - 1;
                }
                break;
            }
            if (d->wd[i].type == W_BUTTON) {
                result = d->wd[i].id;
            } else if (d->wd[i].type == W_INPUT) {
                d->focus = i;
                d->wd[i].changed = 1;
                d->wd[i].pos = (int)strlen(d->wd[i].buf);
            } else if (d->wd[i].type == W_LIST) {
                int r = (e.y - (v->b.y + v->g[i].y + 2)) / ROW_ITEM + d->wd[i].scroll;

                if (r >= 0 && r < d->wd[i].cap) {
                    d->wd[i].pos = r;
                    result = d->wd[i].id;
                }
            }
            break;
        case XE_KEY_DOWN:
            k = xkey(&e);
            if (k == K_NONE)
                break;
            if (k == K_ESC || k == K_F(10)) {
                result = -1;
            } else if (k == K_TAB || k == K_BTAB) {
                focus_step(d, k == K_TAB ? 1 : -1);
            } else if (k == K_ENTER) {
                if (fw && fw->type == W_BUTTON)
                    result = fw->id;
                else if (fw && fw->type == W_LIST)
                    result = fw->id;
                else
                    result = dlg_default(d);
            } else if (fw && fw->type == W_LIST && (k == K_UP || k == K_DOWN || k == K_PGUP ||
                                                    k == K_PGDN || k == K_HOME || k == K_END)) {
                int p = fw->pos;

                p += k == K_UP ? -1 : k == K_DOWN ? 1 : k == K_PGUP ? -fw->h : k == K_PGDN ? fw->h
                   : k == K_HOME ? -p : fw->cap;
                fw->pos = p < 0 ? 0 : p >= fw->cap ? fw->cap - 1 : p;
            } else if (fw && fw->type == W_BUTTON && (k == K_LEFT || k == K_RIGHT)) {
                focus_step(d, k == K_LEFT ? -1 : 1);
            } else if (fw && fw->type == W_INPUT && !(k & K_ALT)) {
                input_key(fw, k);
            } else {
                int c = tolower(k & ~K_ALT), at;
                char cap[64];

                for (i = 0; i < d->n && result == -2; i++) {
                    struct widget *w = &d->wd[i];

                    if (w->type == W_BUTTON && caption(w->text, cap, sizeof cap, &at) == c)
                        result = w->id;
                    if (w->type == W_LIST && w->hotkeys)
                        for (int r = 0; r < w->cap; r++)
                            if (w->hotkeys[r] && tolower((unsigned char)w->hotkeys[r]) == c) {
                                w->pos = r;
                                result = w->id;
                                break;
                            }
                }
            }
            break;
        }
        if (result == -2) {
            dlg_widgets(v);
            x_present();
            x_flush();
        }
    }
    dlg_pop();
    return result;
}

/* Split msg into lines, as labels. */
static int dlg_text(struct dialog *d, char *msg, int y, int center)
{
    char *s = msg, *nl;

    for (;;) {
        nl = strchr(s, '\n');
        if (nl)
            *nl = 0;
        dlg_label(d, y++, s, center);
        if (!nl)
            break;
        s = nl + 1;
    }
    return y;
}

int dlg_query(const char *title, int error, int def, const char *msg, const char *const *buttons,
              int n)
{
    struct dialog d;
    char buf[2048];
    int y, w = 30;

    snprintf(buf, sizeof buf, "%s", msg);
    for (const char *s = msg, *e; *s; s = *e ? e + 1 : e) {
        e = strchr(s, '\n');
        if (!e)
            e = s + strlen(s);
        if (e - s > w)
            w = (int)(e - s);
    }
    dlg_init(&d, title, 0, w + 4, error ? DLG_ERROR : DLG_NORMAL);
    y = dlg_text(&d, buf, 2, 1);
    dlg_buttons(&d, y + 1, 1, buttons, n, def);
    return dlg_run(&d);
}

void dlg_message(const char *title, int error, const char *fmt, ...)
{
    static const char *const ok[] = { "&OK" };
    char msg[2048];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    dlg_query(title, error, 0, msg, ok, 1);
}

int dlg_input_box(const char *title, const char *prompt, char *buf, int cap, int width)
{
    static const char *const btn[] = { "&OK", "&Cancel" };
    struct dialog d;
    char p[1024];
    int y = 2;

    dlg_init(&d, title, 0, width > 0 ? width : 56, DLG_NORMAL);
    if (prompt) {
        snprintf(p, sizeof p, "%s", prompt);
        y = dlg_text(&d, p, y, 0);
    }
    dlg_input(&d, y, buf, cap);
    dlg_buttons(&d, y + 2, 1, btn, 2, 0);
    return dlg_run(&d) == 0;
}

int dlg_list(const char *title, const char *const *items, const char *hotkeys, int n)
{
    struct dialog d;
    struct widget *w;
    int h = n < 14 ? n : 14, width = 24;

    for (int i = 0; i < n; i++)
        if (utf8_len(items[i]) + 8 > width)
            width = utf8_len(items[i]) + 8;
    dlg_init(&d, title, 0, width, DLG_MENU);
    w = dlg_list_box(&d, 2, h, items, hotkeys, n);
    return dlg_run(&d) == 0 ? w->pos : -1;
}

/* For a progress box (fileops' copying): Esc, or a click on its button. */
int key_get(int timeout_ms)
{
    struct x_event e;

    while (next_event(&e, timeout_ms < 0 ? -1 : timeout_ms)) {
        if (e.type == XE_KEY_DOWN) {
            int k = xkey(&e);

            if (k != K_NONE)
                return k;
        } else if (e.type == XE_BUTTON_DOWN && e.button <= 3 && ndlg > 0) {
            struct dlgview *v = &dstack[ndlg - 1];
            int i = dlg_hit(v, e.x, e.y);

            if (i >= 0 && v->d->wd[i].type == W_BUTTON) {
                key_mouse.x = e.x;
                key_mouse.y = e.y;
                key_mouse.button = 0;
                key_mouse.release = 0;
                return K_MOUSE;
            }
        } else if (e.type == XE_CLOSE) {
            return K_QUIT;
        }
        if (timeout_ms == 0)
            break;
    }
    return K_NONE;
}

/* ====================================================================
 * popup menus
 * ==================================================================== */

typedef struct {
    char text[64], side[16];
    int cmd, enabled;           /* cmd 0: a separator */
} pitem;

#define MAXPITEMS 40
#define SEP_H 9

/* Show a menu at (x, y) and return the cmd of the item chosen, or 0. */
static int popup(int x, int y, const char *title, const pitem *it, int n)
{
    int w = title ? x_text_width(f_bold, title) + 30 : 0, h = 8, top, hover = -1, result = -1;
    int iy[MAXPITEMS];
    box b;
    under u;
    char lastkey[64] = "";

    for (int i = 0; i < n; i++) {
        int iw = 28 + x_text_width(f_text, it[i].text) + 28 + x_text_width(f_text, it[i].side) + 12;

        if (iw > w)
            w = iw;
    }
    if (w < 170)
        w = 170;
    top = title ? 4 + ROW_ITEM + 4 : 4;
    h = top;
    for (int i = 0; i < n; i++) {
        iy[i] = h;
        h += it[i].cmd ? ROW_ITEM : SEP_H;
    }
    h += 4;
    if (h > x_height - 8) {             /* too many: leave the rest out */
        while (n > 0 && iy[n - 1] + ROW_ITEM + 4 > x_height - 8)
            n--;
        h = n > 0 ? iy[n - 1] + ROW_ITEM + 4 : top + 4;
    }
    b = (box){ x + w > x_width - 4 ? x_width - 4 - w : x, y + h > x_height - 4 ? x_height - 4 - h : y,
               w, h };
    if (b.x < 4) b.x = 4;
    if (b.y < 4) b.y = 4;

    overlays++;
    under_save(&u, (box){ b.x - 4, b.y - 2, b.w + 8, b.h + 8 });
    x_color(px(mix(RGB_BG, RGB_PANEL, 300)));
    round_rect(b.x - 4, b.y - 2, b.w + 8, b.h + 8, 9);
    x_color(c_track);
    round_rect(b.x - 1, b.y - 1, b.w + 2, b.h + 2, 7);
    x_color(c_popup);
    round_rect(b.x, b.y, b.w, b.h, 6);
    if (title) {
        char t[256];

        to_latin1(t, sizeof t, title);
        text_at(f_bold, b.x + 12, b.y + 4 + (ROW_ITEM - font_h(f_bold)) / 2, b.w - 24, 0,
                TRIM_MID, c_dim, t);
        x_color(c_grid);
        x_fill_rect(b.x + 8, b.y + top - 3, b.w - 16, 1);
    }

    while (result < 0) {
        char key[64];
        struct x_event e;

        snprintf(key, sizeof key, "%d", hover);
        if (strcmp(key, lastkey) != 0) {
            strcpy(lastkey, key);
            for (int i = 0; i < n; i++) {
                int yy = b.y + iy[i], ty = yy + (ROW_ITEM - font_h(f_text)) / 2;

                if (!it[i].cmd) {
                    x_color(c_grid);
                    x_fill_rect(b.x + 8, yy + SEP_H / 2, b.w - 16, 1);
                    continue;
                }
                x_color(c_popup);
                x_fill_rect(b.x + 4, yy, b.w - 8, ROW_ITEM);
                if (i == hover) {
                    x_color(c_sel);
                    round_rect(b.x + 4, yy, b.w - 8, ROW_ITEM, 4);
                }
                text_at(f_text, b.x + 28, ty, b.w - 40, 0, TRIM_END,
                        it[i].enabled ? c_text : c_faint, it[i].text);
                text_at(f_text, b.x + 28, ty, b.w - 40, 1, TRIM_END,
                        it[i].enabled ? c_dim : c_faint, it[i].side);
            }
            x_present();
            x_flush();
        }
        if (!next_event(&e, -1))
            continue;
        if (e.type == XE_MOTION || e.type == XE_BUTTON_DOWN) {
            int h2 = -1;

            for (int i = 0; i < n; i++)
                if (it[i].cmd && it[i].enabled && e.x >= b.x && e.x < b.x + b.w &&
                    e.y >= b.y + iy[i] && e.y < b.y + iy[i] + ROW_ITEM)
                    h2 = i;
            hover = h2;
            if (e.type == XE_BUTTON_DOWN && e.button <= 3) {
                if (h2 >= 0)
                    result = it[h2].cmd;
                else if (!(e.x >= b.x && e.x < b.x + b.w && e.y >= b.y && e.y < b.y + b.h))
                    result = 0;         /* a click outside closes it */
            }
        } else if (e.type == XE_KEY_DOWN) {
            int k = xkey(&e);

            if (k == K_ESC || k == K_F(10)) {
                result = 0;
            } else if (k == K_UP || k == K_DOWN) {
                for (int s = 0; s < n; s++) {
                    hover = hover < 0 ? (k == K_DOWN ? 0 : n - 1)
                          : (hover + (k == K_DOWN ? 1 : -1) + n) % n;
                    if (it[hover].cmd && it[hover].enabled)
                        break;
                }
            } else if (k == K_ENTER && hover >= 0) {
                result = it[hover].cmd;
            } else if (k > 32 && k < 127) {    /* a hotkey in the side column */
                for (int i = 0; i < n; i++)
                    if (it[i].cmd && it[i].enabled && it[i].side[0] == k && !it[i].side[1])
                        result = it[i].cmd;
            }
        } else if (e.type == XE_CLOSE) {
            result = 0;
        }
    }
    under_restore(&u);
    overlays--;
    x_present();
    x_flush();
    return result;
}

/* ====================================================================
 * text views: the file viewer, and command output
 * ==================================================================== */

struct tview {
    char *buf;
    size_t len, cap, max;       /* max: past it the oldest half goes (output) */
    size_t *line;               /* where each shown line starts */
    int nlines, lcap;
    int cols;                   /* wrap width in characters */
    size_t indexed;             /* bytes indexed into lines so far */
    int top, follow, hex, hl;   /* hl: the line found by a search, or -1 */
    int esc;                    /* output: inside an escape sequence */
    int last_top;               /* the top the rows on screen show */
    unsigned ver, drawn_ver;    /* changes to the text; what the screen shows */
    char rowkey[MAXROWS][KEYLEN];
};

static void tv_clear(struct tview *t)
{
    t->len = 0;
    t->nlines = 0;
    t->indexed = 0;
    t->top = 0;
    t->hl = -1;
    t->esc = 0;
    memset(t->rowkey, 0, sizeof t->rowkey);
}

static void tv_reindex(struct tview *t)
{
    t->ver++;
    t->nlines = 0;
    t->indexed = 0;
}

/* Index wrapped lines as far as the text goes. The last line may still
 * grow, so it is indexed again next time. */
static void tv_index(struct tview *t, int cols)
{
    size_t i;
    int col = 0;

    if (cols < 8)
        cols = 8;
    if (cols != t->cols) {
        t->cols = cols;
        tv_reindex(t);
    }
    if (t->hex) {
        t->nlines = (int)((t->len + 15) / 16);
        return;
    }
    if (t->nlines > 0) {                /* re-scan the last, unfinished line */
        t->nlines--;
        i = t->line[t->nlines];
    } else {
        i = 0;
    }
    if (t->len == 0)
        return;
    for (;;) {
        size_t start = i;

        if (t->nlines >= t->lcap) {
            t->lcap = t->lcap ? t->lcap * 2 : 1024;
            t->line = realloc(t->line, sizeof *t->line * (size_t)t->lcap);
            if (!t->line) {
                t->lcap = t->nlines = 0;
                return;
            }
        }
        t->line[t->nlines++] = start;
        col = 0;
        while (i < t->len && t->buf[i] != '\n') {
            unsigned char c = (unsigned char)t->buf[i];

            if ((c & 0xC0) != 0x80) {
                if (col >= cols)
                    break;
                col = c == '\t' ? (col + 8) & ~7 : col + 1;
            }
            i++;
        }
        if (i < t->len && t->buf[i] == '\n')
            i++;
        if (i >= t->len)
            break;
    }
    t->indexed = t->len;
}

/* Line k as text to show (Latin-1, tabs expanded). */
static void tv_line(struct tview *t, int k, char *out, size_t cap)
{
    size_t n = 0;

    if (t->hex) {
        size_t off = (size_t)k * 16;
        char asc[17];
        int m = 0;

        n = (size_t)snprintf(out, cap, "%08lx  ", (unsigned long)off);
        for (int j = 0; j < 16; j++) {
            if (off + (size_t)j < t->len) {
                unsigned char c = (unsigned char)t->buf[off + (size_t)j];

                n += (size_t)snprintf(out + n, cap - n, "%02x %s", c, j == 7 ? " " : "");
                asc[m++] = c >= 32 && c < 127 ? (char)c : '.';
            } else {
                n += (size_t)snprintf(out + n, cap - n, "   %s", j == 7 ? " " : "");
            }
        }
        asc[m] = 0;
        snprintf(out + n, cap - n, " %s", asc);
        return;
    }
    if (k < 0 || k >= t->nlines) {
        out[0] = 0;
        return;
    }
    {
        size_t i = t->line[k], end = k + 1 < t->nlines ? t->line[k + 1] : t->len;
        int col = 0;

        while (i < end && t->buf[i] != '\n' && n + 2 < cap) {
            const char *s = t->buf + i;
            unsigned c;

            if ((unsigned char)*s < 0x80) {
                c = (unsigned char)*s;
                i++;
            } else {
                const char *p = s;

                c = utf8_next(&p);
                i += (size_t)(p - s);
            }
            if (c == '\t') {
                do
                    out[n++] = ' ';
                while (++col % 8 && n + 2 < cap);
                continue;
            }
            out[n++] = (char)(c < 32 || c == 127 ? '.' : c < 256 ? c : '?');
            col++;
        }
    }
    out[n] = 0;
}

/* Draw the view's rows in box b; rows that already show the right line
 * are left alone, and a scroll is one copy on the server. */
static int tv_rows(box b) { return b.h / (font_h(f_mono) + 1); }

static void tv_draw(struct tview *t, box b, unsigned bg)
{
    int rh = font_h(f_mono) + 1, rows = tv_rows(b), cw = x_text_width(f_mono, "0");
    char line[1024], key[KEYLEN];

    if (rows > MAXROWS)
        rows = MAXROWS;
    tv_index(t, b.w / (cw ? cw : 7));
    if (t->follow)
        t->top = t->nlines - rows;
    if (t->top > t->nlines - rows)
        t->top = t->nlines - rows;
    if (t->top < 0)
        t->top = 0;

    /* a small scroll: move what is there, then draw the rest */
    if (t->rowkey[0][0] && t->top == t->last_top && t->ver == t->drawn_ver)
        return;                         /* nothing to do */
    t->drawn_ver = t->ver;
    if (t->rowkey[0][0] && t->top != t->last_top && abs(t->top - t->last_top) < rows) {
        int d = t->top - t->last_top, keep = rows - abs(d);

        if (d > 0) {
            x_blit(x_frame(), b.x, b.y + d * rh, b.w, keep * rh, b.x, b.y);
            memmove(t->rowkey[0], t->rowkey[d], (size_t)keep * KEYLEN);
            for (int r = keep; r < rows; r++)
                t->rowkey[r][0] = 0;
        } else {
            x_blit(x_frame(), b.x, b.y, b.w, keep * rh, b.x, b.y - d * rh);
            memmove(t->rowkey[-d], t->rowkey[0], (size_t)keep * KEYLEN);
            for (int r = 0; r < -d; r++)
                t->rowkey[r][0] = 0;
        }
    }
    t->last_top = t->top;

    for (int r = 0; r < rows; r++) {
        int k = t->top + r, yy = b.y + r * rh;

        tv_line(t, k, line, sizeof line);
        snprintf(key, sizeof key, "%d|%s", k == t->hl, line);
        if (strcmp(key, t->rowkey[r]) == 0)
            continue;
        snprintf(t->rowkey[r], KEYLEN, "%s", key);
        x_color(k == t->hl ? c_sel : bg);
        x_fill_rect(b.x - 4, yy, b.w + 8, rh);
        if (line[0]) {
            x_text_font(f_mono);
            x_color(c_text);
            x_text(b.x, yy + x_font_ascent(f_mono), line);
        }
    }
}

static void tv_forget(struct tview *t)
{
    memset(t->rowkey, 0, sizeof t->rowkey);
}

/* Find text (ignoring case) after line `from`; returns the line or -1. */
static int tv_search(struct tview *t, const char *what, int from)
{
    size_t n = strlen(what);

    if (n == 0 || t->hex)
        return -1;
    for (int k = from + 1; k < t->nlines; k++) {
        size_t i = t->line[k], end = k + 1 < t->nlines ? t->line[k + 1] : t->len;

        for (; i + n <= end; i++)
            if (strncasecmp(t->buf + i, what, n) == 0)
                return k;
    }
    return -1;
}

/* ====================================================================
 * the window: panels, output, command line
 * ==================================================================== */

static struct panel pan[2];
static int act;                         /* the active panel */

/* What a row shows, to tell whether it must be drawn again: cheap to
 * fill in and compare, where formatting the row is not. */
struct rowst {
    int valid, present, flags, cur, active, hl, w;
    unsigned hash, mode;
    long long size;
    long mtime;
};

static unsigned name_hash(const char *s)
{
    unsigned h = 5381;

    while (*s)
        h = h * 33 + (unsigned char)*s++;
    return h;
}

struct pgeo {
    box b;
    int list_y, rows, row_h;
    int name_x, size_x, size_w, date_x, date_w;
    int ncrumb, crumb_x[48], crumb_w[48], crumb_end[48];
    int crumb_y, head_y, foot_y;
    struct rowst rs[MAXROWS];  /* what each row on screen shows */
    char titlekey[KEYLEN], headkey[64], footkey[KEYLEN], barkey[64];
    int last_top;
};
static struct pgeo geo[2];

static box b_out, b_out_text, b_cmd;
static char cmd_key[KEYLEN], header_key[KEYLEN];
static int layout_gen;

static char cmdline[1024];
static int cmdpos;
static char status_msg[256];            /* shown on the command line for a moment */
static long long status_until;
static char prev_dir[PATH_MAX];

/* Tabs: file manager tabs, each with two panels of its own, and terminal
 * tabs, each running a command. pan[], act and the command line belong to
 * one files tab at a time (the live one); the others keep theirs here. */
#define MAX_TABS 16
enum { TAB_FILES, TAB_TERM };
struct tab {
    int kind;
    struct panel pan[2];                /* a files tab's, while not live */
    int act, cmdpos;
    char cmdline[1024];
    struct term t;                      /* a terminal tab's */
    int running, status, spoke;         /* spoke: has written something */
    long long started;
    struct tab *from;                   /* the files tab it was started from */
    char title[256];
};
static struct tab *tabs[MAX_TABS];
static int ntabs, cur_tab;
static struct tab *live;
static struct term term_proto;          /* fonts and colours, for each terminal */

#define CUR (tabs[cur_tab])
#define ON_TERM (CUR->kind == TAB_TERM)
#define CUR_RUNNING (ON_TERM && CUR->running)

/* the tab bar, as last drawn */
static box tb_box[MAX_TABS], tb_close[MAX_TABS], b_plus, b_gear;
static int tb_has_close[MAX_TABS], tab_hover = -1;

/* a drag in progress */
static struct {
    int on, active, panel, row, mode;   /* mode: 0 drag files, 1 mark rows */
    int x0, y0, px, py, mark_to, last_row;
    under ghost, target;
    int target_kind, target_panel, target_row;  /* 0 none, 1 panel, 2 folder, 3 command line */
    char label[160];
} drag;

static int shift_anchor[2] = { -1, -1 };

static void say(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(status_msg, sizeof status_msg, fmt, ap);
    va_end(ap);
    status_until = now_ms() + 4000;
}

/* Shell-quote s onto the end of buf. */
static void quote_onto(char *buf, size_t cap, const char *s)
{
    size_t n = strlen(buf);

    if (n + 3 >= cap)
        return;
    buf[n++] = '\'';
    for (; *s && n + 5 < cap; s++) {
        if (*s == '\'') {
            memcpy(buf + n, "'\\''", 4);
            n += 4;
        } else {
            buf[n++] = *s;
        }
    }
    buf[n++] = '\'';
    buf[n] = 0;
}

/* ---- layout ---- */

static int head_h(void) { return font_h(f_head) + 8; }

static void layout(void)
{
    int W = x_width, H = x_height, y0 = M + head_h() + G, cmd_h = 34;
    int bottom = H - M - cmd_h - G, half = (W - 2 * M - G) / 2, ph = bottom - y0;

    /* a terminal tab has the whole window below the tabs */
    b_out = (box){ M, y0, W - 2 * M, H - M - y0 };
    b_out_text = (box){ b_out.x + 10, b_out.y + 8, b_out.w - 20, b_out.h - 16 };
    b_cmd = (box){ M, H - M - cmd_h, W - 2 * M, cmd_h };
    for (int i = 0; i < 2; i++) {
        struct pgeo *g = &geo[i];
        int x = M + i * (half + G), w = i ? W - 2 * M - G - half : half;

        g->b = (box){ x, y0, w, ph };
        g->row_h = font_h(f_text) + 7;
        g->crumb_y = y0 + P - 2;
        g->head_y = g->crumb_y + font_h(f_bold) + 10;
        g->list_y = g->head_y + font_h(f_text) + 6;
        g->foot_y = y0 + ph - P - font_h(f_text);
        g->rows = (g->foot_y - 8 - g->list_y) / g->row_h;
        if (g->rows < 1)
            g->rows = 1;
        if (g->rows > MAXROWS)
            g->rows = MAXROWS;
        g->date_w = w >= 420 ? x_text_width(f_text, "Nov 16 22:55") + 4 : 0;
        g->size_w = w >= 300 ? x_text_width(f_text, "1023.9M") + 4 : 0;
        g->date_x = x + w - P - 10 - g->date_w;
        g->size_x = g->date_x - (g->date_w ? 16 : 0) - g->size_w;
        g->name_x = x + P + 24;
        pan[i].h = g->rows + 5;         /* so panel.c scrolls by our rows */
        pan[i].w = w / 8;
        g->last_top = -1;
        panel_goto(&pan[i], pan[i].cur);
    }
    scr_cols = W / 8;
    scr_rows = H / 16;
}

/* Lay out again and draw everything that doesn't change. */
static void relayout(void)
{
    layout();
    layout_gen++;
    x_color(c_bg);
    x_fill_rect(0, 0, x_width, x_height);
    x_line_width(1);
    cmd_key[0] = header_key[0] = 0;
    if (ON_TERM) {
        x_color(c_panel);
        round_rect(b_out.x, b_out.y, b_out.w, b_out.h, RADIUS);
        term_place(&CUR->t, b_out_text.x, b_out_text.y, b_out_text.w, b_out_text.h);
        return;
    }
    for (int i = 0; i < 2; i++) {
        struct pgeo *g = &geo[i];

        x_color(c_panel);
        round_rect(g->b.x, g->b.y, g->b.w, g->b.h, RADIUS);
        x_color(c_grid);
        x_fill_rect(g->b.x + P, g->list_y - 3, g->b.w - 2 * P, 1);
        x_fill_rect(g->b.x + P, g->foot_y - 6, g->b.w - 2 * P, 1);
        memset(g->rs, 0, sizeof g->rs);
        g->titlekey[0] = g->headkey[0] = g->footkey[0] = g->barkey[0] = 0;
    }
    x_color(c_panel);
    round_rect(b_cmd.x, b_cmd.y, b_cmd.w, b_cmd.h, RADIUS);
}

/* ---- drawing ---- */

/* ---- the tab bar ---- */

static void tab_title(const struct tab *t, char *out, size_t cap)
{
    const struct panel *p;

    if (t->kind == TAB_TERM) {
        to_latin1(out, cap, t->title);
        return;
    }
    p = t == live ? &pan[act] : &t->pan[t->act];
    to_latin1(out, cap, p->path[1] ? path_base(p->path) : "/");
}

/* A finished command that went wrong: its icon is red (or grey if it
 * was interrupted). */
static unsigned tab_icon_color(const struct tab *t, int on)
{
    if (t->kind == TAB_TERM && !t->running) {
        if (WIFSIGNALED(t->status) && WTERMSIG(t->status) == SIGINT)
            return c_faint;
        if (!WIFEXITED(t->status) || WEXITSTATUS(t->status) != 0)
            return px(ACC_RED);
    }
    if (t->kind == TAB_FILES)
        return px(ACC_BLUE);
    return on ? c_text : c_dim;
}

/* 16 x 14 at (x, y): a folder, or a terminal (hollow once it has ended). */
static void tab_icon(int x, int y, const struct tab *t, unsigned color, unsigned bg)
{
    x_color(color);
    if (t->kind == TAB_FILES) {
        round_rect(x, y + 1, 7, 4, 1);
        round_rect(x, y + 3, 15, 10, 2);
        x_color(bg);
        x_fill_rect(x + 2, y + 6, 11, 1);
        return;
    }
    round_rect(x, y + 1, 16, 12, 2);
    x_color(bg);
    x_fill_rect(x + 1, y + 2, 14, 10);
    x_color(color);
    x_line(x + 4, y + 4, x + 6, y + 6);
    x_line(x + 6, y + 6, x + 4, y + 8);
    if (t->kind == TAB_TERM && t->running)
        x_fill_rect(x + 8, y + 8, 4, 1);
}

/* ---- glyphs: the gear, + and x, drawn smooth ----
 *
 * Each is a shape on a 20 x 20 grid around (0, 0). It is rendered once
 * for each size and pair of colours, 4 x 4 samples a pixel blended from
 * the background to the colour, kept on the server and copied from
 * there after. */
enum { GLYPH_GEAR, GLYPH_PLUS, GLYPH_CLOSE };

static const rgb RGB_DIM = { 139, 148, 158 };

static float absf(float v) { return v < 0 ? -v : v; }

/* (The + is 2 pixels wide at 16: its edges fall on pixel edges.) */
static int glyph_in(int g, float x, float y)
{
    static const float dir[3][2] = { { 1, 0 }, { 0.5f, 0.8660254f }, { -0.5f, 0.8660254f } };
    float d2 = x * x + y * y;

    switch (g) {
    case GLYPH_GEAR:                    /* 6 teeth on a ring */
        if (d2 < 3.4f * 3.4f || d2 > 9.6f * 9.6f)
            return 0;
        if (d2 <= 6.8f * 6.8f)
            return 1;
        for (int k = 0; k < 3; k++)     /* a bar through the middle: two teeth */
            if (absf(y * dir[k][0] - x * dir[k][1]) <= 2.0f)
                return 1;
        return 0;
    case GLYPH_PLUS:
        return (absf(x) <= 1.25f && absf(y) <= 7.5f) || (absf(y) <= 1.25f && absf(x) <= 7.5f);
    default: {                          /* GLYPH_CLOSE: a thinner + turned 45 degrees */
        float u = (x + y) * 0.70710678f, v = (x - y) * 0.70710678f;

        return (absf(u) <= 0.75f && absf(v) <= 6.0f) || (absf(v) <= 0.75f && absf(u) <= 6.0f);
    }
    }
}

static void draw_glyph(int g, int x, int y, int size, rgb fg, rgb bg)
{
    static struct { int g, size; rgb fg, bg; unsigned canvas; } cache[24];
    static int ncache;
    unsigned pix[32 * 32];
    int i;

    if (size > 32)
        size = 32;
    for (i = 0; i < ncache; i++)
        if (cache[i].g == g && cache[i].size == size && !memcmp(&cache[i].fg, &fg, sizeof fg) &&
            !memcmp(&cache[i].bg, &bg, sizeof bg))
            break;
    if (i == ncache) {
        if (ncache == (int)(sizeof cache / sizeof cache[0])) {
            x_free_canvas(cache[0].canvas);
            memmove(cache, cache + 1, sizeof cache - sizeof cache[0]);
            ncache--;
        }
        i = ncache++;
        cache[i].g = g;
        cache[i].size = size;
        cache[i].fg = fg;
        cache[i].bg = bg;
        for (int py = 0; py < size; py++)
            for (int qx = 0; qx < size; qx++) {
                int n = 0;

                for (int sy = 0; sy < 4; sy++)
                    for (int sx = 0; sx < 4; sx++)
                        n += glyph_in(g, ((float)qx + (sx + 0.5f) / 4) * 20 / size - 10,
                                      ((float)py + (sy + 0.5f) / 4) * 20 / size - 10);
                pix[py * size + qx] = px(mix(bg, fg, n * 1000 / 16));
            }
        cache[i].canvas = x_canvas(size, size);
        x_target(cache[i].canvas);
        x_image(0, 0, size, size, pix, size);
        x_target(0);                    /* back to the frame */
    }
    x_blit(cache[i].canvas, 0, 0, size, size, x, y);
}

/* Where the tabs go. They share the row up to 200 pixels each; when that
 * would make them too narrow to read, the one shown keeps its name and
 * the rest shrink to their icons, and the one under the pointer opens up
 * (to the right, so it stays under the pointer). */
static void tabs_layout(void)
{
    int bh = head_h(), y = M, gap = 6, x = M, avail, w, chip, big = 180, nfiles = 0;

    b_gear = (box){ x_width - M - bh, y, bh, bh };
    avail = b_gear.x - 12 - (bh + gap) - M;     /* the + goes after the tabs */
    w = (avail + gap) / ntabs - gap;
    if (w > 200)
        w = 200;
    chip = 0;
    if (w < 120 && ntabs > 1) {
        int nchips = ntabs > 2 ? ntabs - 2 : 1;     /* room for the hovered one too */

        chip = (avail - 2 * big - gap * (ntabs - 1)) / nchips;
        if (chip > 44)
            chip = 44;
        if (chip < 26) {
            chip = 26;
            big = (avail - nchips * chip - gap * (ntabs - 1)) / 2;
            if (big < 60)
                big = 60;
        }
    }
    for (int i = 0; i < ntabs; i++)
        nfiles += tabs[i]->kind == TAB_FILES;
    for (int i = 0; i < ntabs; i++) {
        int wi = !chip ? w : i == cur_tab || i == tab_hover ? big : chip;

        tb_box[i] = (box){ x, y, wi, bh };
        tb_has_close[i] = wi >= 60 && (tabs[i]->kind == TAB_TERM || nfiles > 1);
        tb_close[i] = (box){ x + wi - 22, y + (bh - 18) / 2, 18, 18 };
        x += wi + gap;
    }
    b_plus = (box){ x, y, bh, bh };
}

static void draw_tabbar(void)
{
    char key[KEYLEN], t[300], all[MAX_TABS * 24];
    int len = 0;

    tabs_layout();
    for (int i = 0; i < ntabs; i++) {
        tab_title(tabs[i], t, sizeof t);
        len += snprintf(all + len, sizeof all - (size_t)len, "%u.%u.%d|", name_hash(t),
                        tab_icon_color(tabs[i], i == cur_tab), tb_box[i].w);
    }
    snprintf(key, sizeof key, "%u|%d|%d|%d|%d|%d", name_hash(all), ntabs, cur_tab, tab_hover,
             x_width, layout_gen);
    if (strcmp(key, header_key) == 0)
        return;
    strcpy(header_key, key);
    x_color(c_bg);
    x_fill_rect(0, 0, x_width, M + head_h() + 2);
    for (int i = 0; i < ntabs; i++) {
        box b = tb_box[i];
        int on = i == cur_tab, hov = i == tab_hover;
        rgb bgc = on ? mix(RGB_PANEL, ACC_BLUE, 330) : hov ? mix(RGB_PANEL, RGB_TEXT, 90) : RGB_PANEL;
        unsigned bg = px(bgc);
        int tx = b.x + 30, tw = b.w - 30 - (tb_has_close[i] ? 26 : 8);

        x_color(bg);
        round_rect(b.x, b.y, b.w, b.h, 6);
        tab_icon(b.w < 60 ? b.x + (b.w - 16) / 2 : b.x + 10, b.y + (b.h - 14) / 2, tabs[i],
                 tab_icon_color(tabs[i], on), bg);
        if (tw > 8) {
            tab_title(tabs[i], t, sizeof t);
            text_at(f_text, tx, b.y + (b.h - font_h(f_text)) / 2, tw, 0, TRIM_END,
                    on ? c_text : c_dim, t);
        }
        if (tb_has_close[i])
            draw_glyph(GLYPH_CLOSE, tb_close[i].x, tb_close[i].y, tb_close[i].w,
                       on || hov ? RGB_TEXT : RGB_DIM, bgc);
    }
    draw_glyph(GLYPH_PLUS, b_plus.x + (b_plus.w - 16) / 2, b_plus.y + (b_plus.h - 16) / 2, 16,
               RGB_DIM, RGB_BG);
    draw_glyph(GLYPH_GEAR, b_gear.x + (b_gear.w - 18) / 2, b_gear.y + (b_gear.h - 18) / 2, 18,
               RGB_DIM, RGB_BG);
}

static void fmt_sz(char *dst, size_t cap, long long b)
{
    const char *u = "BKMGT";
    long long v = b * 10;
    int k = 0;

    while (v >= 10240 && k < 4) {
        v /= 1024;
        k++;
    }
    if (k == 0)
        snprintf(dst, cap, "%lld", b);
    else if (v < 100)
        snprintf(dst, cap, "%lld.%lld%c", v / 10, v % 10, u[k]);
    else
        snprintf(dst, cap, "%lld%c", v / 10, u[k]);
}

static void mode_string(char *s, const struct entry *e)
{
    const char *rwx = "rwxrwxrwx";

    s[0] = S_ISDIR(e->mode) ? 'd' : S_ISLNK(e->mode) || (e->flags & E_LINK) ? 'l'
         : S_ISCHR(e->mode) ? 'c' : S_ISBLK(e->mode) ? 'b' : S_ISFIFO(e->mode) ? 'p'
         : S_ISSOCK(e->mode) ? 's' : '-';
    for (int i = 0; i < 9; i++)
        s[i + 1] = e->mode & (0400 >> i) ? rwx[i] : '-';
    s[10] = 0;
}

/* A little picture of the entry's type. */
static void draw_icon(int x, int y, const struct entry *e, unsigned color)
{
    if (entry_is_dir(e)) {
        x_color(color);
        round_rect(x, y + 1, 7, 4, 1);
        round_rect(x, y + 3, 15, 10, 2);
        x_color(c_panel);
        x_fill_rect(x + 2, y + 6, 11, 1);
    } else {
        x_color(color);
        round_rect(x + 2, y, 11, 14, 2);
        x_color(px(mix(RGB_PANEL, RGB_TEXT, 120)));
        x_fill_rect(x + 4, y + 4, 7, 1);
        x_fill_rect(x + 4, y + 7, 7, 1);
        x_fill_rect(x + 4, y + 10, 5, 1);
        if (e->flags & E_LINK) {
            x_color(c_text);
            x_fill_rect(x + 9, y + 9, 5, 5);
        }
    }
}

static unsigned entry_px(const struct entry *e)
{
    int c = entry_color(e);

    return c >= 0 && c < 16 ? type_px[c] : c_text;
}

static int drop_row_hl(int pi, int idx)
{
    return drag.active && drag.mode == 0 && drag.target_kind == 2 && drag.target_panel == pi &&
           drag.target_row == idx;
}

static void draw_panel(int pi)
{
    struct panel *p = &pan[pi];
    struct pgeo *g = &geo[pi];
    int active = pi == act, x = g->b.x + P, w = g->b.w - 2 * P;
    char key[KEYLEN], t[PATH_MAX], buf[600];

    /* the path, as parts that can be clicked */
    snprintf(key, sizeof key, "%d|%s|%lld", active, p->path, p->fs_free);
    if (strcmp(key, g->titlekey) != 0) {
        char fr[32] = "", segs[PATH_MAX];
        int cx, room, fw;

        strcpy(g->titlekey, key);
        x_color(c_panel);
        x_fill_rect(x - 2, g->crumb_y - 2, w + 4, font_h(f_bold) + 6);
        x_color(active ? px(ACC_BLUE) : c_faint);
        round_rect(x, g->crumb_y + 2, 4, x_font_ascent(f_bold) - 1, 2);
        if (p->fs_free >= 0) {
            char a[16];

            fmt_sz(a, sizeof a, p->fs_free);
            snprintf(fr, sizeof fr, "%s free", a);
        }
        fw = fr[0] ? x_text_width(f_text, fr) + 8 : 0;
        text_at(f_text, x, g->crumb_y + 1, w, 1, TRIM_END, c_dim, fr);
        room = w - 12 - fw;
        to_latin1(segs, sizeof segs, p->path);
        /* drop leading parts until it fits, mc-style "..." in front */
        {
            int start = 0, n = (int)strlen(segs);

            while (x_text_width(f_bold, segs + start) + (start ? x_text_width(f_bold, "...") : 0)
                       > room && start < n) {
                start++;
                while (start < n && segs[start] != '/')
                    start++;
            }
            cx = x + 10;
            g->ncrumb = 0;
            if (start > 0) {
                text_at(f_bold, cx, g->crumb_y, 40, 0, TRIM_END, c_faint, "...");
                cx += x_text_width(f_bold, "...");
            }
            for (int i = start; i < n && g->ncrumb < 48;) {
                int j = i + 1, sw;
                char part[256];

                while (j < n && segs[j] != '/')
                    j++;
                snprintf(part, sizeof part, "%.*s", j - i, segs + i);
                if (i == 0 && j == 1)
                    snprintf(part, sizeof part, "/");
                sw = x_text_width(f_bold, part);
                if (cx + sw > x + 10 + room)
                    sw = x + 10 + room - cx;
                text_at(f_bold, cx, g->crumb_y, sw, 0, TRIM_END,
                        j >= n ? (active ? c_text : c_dim) : c_dim, part);
                g->crumb_x[g->ncrumb] = cx;
                g->crumb_w[g->ncrumb] = sw;
                g->crumb_end[g->ncrumb] = j;
                g->ncrumb++;
                cx += sw;
                i = j;
            }
        }
    }

    /* column headings */
    snprintf(key, sizeof key, "%d|%d|%d", p->sort, p->reverse, layout_gen);
    if (strcmp(key, g->headkey) != 0) {
        int hy = g->head_y;

        strcpy(g->headkey, key);
        x_color(c_panel);
        x_fill_rect(x, hy, w, font_h(f_text) + 2);
        text_at(f_text, g->name_x, hy, 200, 0, TRIM_END, p->sort == SORT_NAME ? c_text : c_faint,
                "Name");
        if (p->sort == SORT_NAME)
            triangle(g->name_x + x_text_width(f_text, "Name") + 5, hy + 5, p->reverse,
                     px(ACC_BLUE));
        if (g->size_w) {
            text_at(f_text, g->size_x, hy, g->size_w, 1, TRIM_END,
                    p->sort == SORT_SIZE ? c_text : c_faint, "Size");
            if (p->sort == SORT_SIZE)
                triangle(g->size_x + g->size_w - x_text_width(f_text, "Size") - 12, hy + 5,
                         p->reverse, px(ACC_BLUE));
        }
        if (g->date_w) {
            text_at(f_text, g->date_x, hy, g->date_w, 1, TRIM_END,
                    p->sort == SORT_MTIME ? c_text : c_faint, "Modified");
            if (p->sort == SORT_MTIME)
                triangle(g->date_x + g->date_w - x_text_width(f_text, "Modified") - 12, hy + 5,
                         p->reverse, px(ACC_BLUE));
        }
    }

    /* the rows; a scroll moves what's there first */
    if (g->last_top >= 0 && p->top != g->last_top && abs(p->top - g->last_top) < g->rows &&
        g->rs[0].valid) {
        int d = p->top - g->last_top, keep = g->rows - abs(d), rh = g->row_h;

        if (d > 0) {
            x_blit(x_frame(), x - 4, g->list_y + d * rh, w + 4, keep * rh, x - 4, g->list_y);
            memmove(&g->rs[0], &g->rs[d], (size_t)keep * sizeof g->rs[0]);
            memset(&g->rs[keep], 0, (size_t)(g->rows - keep) * sizeof g->rs[0]);
        } else {
            x_blit(x_frame(), x - 4, g->list_y, w + 4, keep * rh, x - 4, g->list_y - d * rh);
            memmove(&g->rs[-d], &g->rs[0], (size_t)keep * sizeof g->rs[0]);
            memset(&g->rs[0], 0, (size_t)(-d) * sizeof g->rs[0]);
        }
    }
    g->last_top = p->top;
    for (int r = 0; r < g->rows; r++) {
        int i = p->top + r, yy = g->list_y + r * g->row_h, cur = i == p->cur;
        struct entry *e = i < p->n ? &p->e[i] : NULL;
        char sz[24] = "", dt[24] = "";
        int marked = e && (e->flags & E_MARKED), hl = e && drop_row_hl(pi, i);
        unsigned bg;
        struct rowst st;

        memset(&st, 0, sizeof st);
        st.valid = 1;
        st.w = g->b.w;
        if (e) {
            st.present = 1;
            st.hash = name_hash(e->name);
            st.size = e->size;
            st.mtime = (long)e->mtime;
            st.mode = (unsigned)e->mode;
            st.flags = e->flags;
            st.cur = cur;
            st.active = active;
            st.hl = hl;
        }
        if (memcmp(&st, &g->rs[r], sizeof st) == 0)
            continue;
        g->rs[r] = st;
        x_color(c_panel);
        x_fill_rect(x - 4, yy, w + 4, g->row_h);
        if (!e)
            continue;
        to_latin1(t, sizeof t, e->name);
        if (e->flags & E_UPDIR)
            snprintf(sz, sizeof sz, "up");
        else if (!entry_is_dir(e))
            fmt_sz(sz, sizeof sz, e->size);
        if (g->date_w && !(e->flags & E_UPDIR))
            fmt_time(dt, e->mtime);
        bg = cur ? (active ? c_sel : c_sel_dim) : marked ? c_mark_bg : c_panel;
        if (bg != c_panel || hl) {
            x_color(hl ? px(mix(RGB_PANEL, ACC_GREEN, 450)) : bg);
            round_rect(x - 4, yy, w + 2, g->row_h, 4);
        }
        if (marked) {
            x_color(px(ACC_MARK));
            round_rect(x - 2, yy + 4, 3, g->row_h - 8, 1);
        }
        draw_icon(x + 4, yy + (g->row_h - 14) / 2, e,
                  entry_is_dir(e) ? px(mix(ACC_BLUE, RGB_PANEL, cur ? 0 : 150)) : entry_px(e));
        {
            int nw = (g->size_w ? g->size_x - 10 : g->b.x + g->b.w - P - 12) - g->name_x;
            int f = entry_is_dir(e) ? f_bold : f_text;

            text_at(f, g->name_x, yy + (g->row_h - font_h(f)) / 2, nw, 0, TRIM_MID,
                    marked ? px(ACC_MARK) : entry_is_dir(e) ? c_text : entry_px(e), t);
        }
        if (g->size_w)
            text_at(f_text, g->size_x, yy + (g->row_h - font_h(f_text)) / 2, g->size_w, 1,
                    TRIM_END, cur && active ? c_text : c_dim, sz);
        if (g->date_w)
            text_at(f_text, g->date_x, yy + (g->row_h - font_h(f_text)) / 2, g->date_w, 1,
                    TRIM_END, cur && active ? c_text : c_dim, dt);
    }

    /* the scroll bar */
    {
        int bx = g->b.x + g->b.w - P + 2, by = g->list_y, bh = g->rows * g->row_h;
        int th = p->n > g->rows ? bh * g->rows / p->n : 0, ty = 0;

        if (th && th < 16)
            th = 16;
        if (th)
            ty = (int)((long)(bh - th) * p->top / (p->n - g->rows > 0 ? p->n - g->rows : 1));
        snprintf(key, sizeof key, "%d|%d|%d", th, ty, layout_gen);
        if (strcmp(key, g->barkey) != 0) {
            strcpy(g->barkey, key);
            x_color(c_panel);
            x_fill_rect(bx, by, 5, bh);
            if (th) {
                x_color(c_track);
                round_rect(bx, by, 5, bh, 2);
                x_color(c_dim);
                round_rect(bx, by + ty, 5, th, 2);
            }
        }
    }

    /* the footer: marked files, or the current one */
    {
        struct entry *e = panel_current(p);
        char left[300], right[300] = "";

        if (p->nmarked) {
            char sz[24];

            fmt_sz(sz, sizeof sz, p->marked_bytes);
            snprintf(left, sizeof left, "%d marked, %s", p->nmarked, sz);
        } else {
            snprintf(left, sizeof left, "%d item%s", p->n - (p->n && (p->e[0].flags & E_UPDIR)),
                     p->n == 2 ? "" : "s");
        }
        if (e && !(e->flags & E_UPDIR)) {
            char ms[12];

            mode_string(ms, e);
            if (e->flags & E_LINK) {
                char full[PATH_MAX], tgt[PATH_MAX];
                ssize_t n;

                tgt[0] = 0;
                if (path_join(full, sizeof full, p->path, e->name) == 0 &&
                    (n = readlink(full, tgt, sizeof tgt - 1)) >= 0)
                    tgt[n] = 0;
                snprintf(buf, sizeof buf, "-> %s   %s", tgt, ms);
            } else {
                snprintf(buf, sizeof buf, "%s", ms);
            }
            to_latin1(right, sizeof right, buf);
        }
        snprintf(key, sizeof key, "%d|%s|%s|%d", p->nmarked > 0, left, right, layout_gen);
        if (strcmp(key, g->footkey) != 0) {
            int lw = x_text_width(f_text, left) + 16;

            strcpy(g->footkey, key);
            x_color(c_panel);
            x_fill_rect(x, g->foot_y, w, font_h(f_text) + 1);
            text_at(f_text, x, g->foot_y, w, 0, TRIM_END, p->nmarked ? px(ACC_MARK) : c_dim,
                    left);
            x_text_font(f_mono);
            text_at(f_mono, x + lw, g->foot_y, w - lw, 1, TRIM_START, c_faint, right);
        }
    }
}

static void draw_cmdline(void)
{
    char key[KEYLEN * 2], prompt[PATH_MAX], t[1100], before[1100];
    int y = b_cmd.y + (b_cmd.h - font_h(f_mono)) / 2, x = b_cmd.x + P, pw, room;
    int show_status = status_msg[0] && now_ms() < status_until;

    if (!show_status)
        status_msg[0] = 0;
    to_latin1(prompt, sizeof prompt, pan[act].path);
    snprintf(key, sizeof key, "%s|%s|%d|%s|%d|%d", prompt, cmdline, cmdpos, status_msg,
             drag.target_kind == 3, layout_gen);
    if (strcmp(key, cmd_key) == 0)
        return;
    snprintf(cmd_key, sizeof cmd_key, "%s", key);
    x_color(drag.active && drag.target_kind == 3 ? px(mix(RGB_PANEL, ACC_GREEN, 300)) : c_panel);
    round_rect(b_cmd.x, b_cmd.y, b_cmd.w, b_cmd.h, RADIUS);
    {
        char pr[PATH_MAX + 4], fitted[600];

        fit(fitted, sizeof fitted, f_mono, prompt, b_cmd.w / 3, TRIM_START);
        snprintf(pr, sizeof pr, "%s $", fitted);
        text_at(f_mono, x, y, b_cmd.w / 3 + 30, 0, TRIM_END, px(ACC_GREEN), pr);
        pw = x_text_width(f_mono, pr) + 10;
    }
    room = b_cmd.w - 2 * P - pw;
    if (show_status) {
        text_at(f_text, x + pw, y, room, 0, TRIM_END, px(ACC_AMBER), status_msg);
        return;
    }
    to_latin1(t, sizeof t, cmdline);
    {
        char tmp[1100];
        int start = 0;

        snprintf(tmp, sizeof tmp, "%.*s", cmdpos, cmdline);
        to_latin1(before, sizeof before, tmp);
        while (x_text_width(f_mono, before + start) > room - 10 && before[start])
            start++;
        text_at(f_mono, x + pw, y, room, 0, TRIM_END, c_text, t + start);
        x_color(px(ACC_BLUE));
        x_fill_rect(x + pw + x_text_width(f_mono, before + start), y - 1, 2, font_h(f_mono) + 2);
    }
}

static void draw_all(void)
{
    if (overlays)
        return;
    draw_tabbar();
    if (ON_TERM) {
        CUR->t.focus = CUR->running;
        term_draw(&CUR->t);
        return;
    }
    draw_panel(0);
    draw_panel(1);
    draw_cmdline();
}

static void drag_overlay_clear(void);
static void drag_show(int x, int y);

static void present(void)
{
    int dragging = drag.active && drag.mode == 0;

    /* At most one frame on its way: wait for the last one, not this one,
     * so drawing overlaps the trip to the server. */
    x_fence_wait();
    if (dragging)
        drag_overlay_clear();
    draw_all();
    if (dragging)
        drag_show(drag.px, drag.py);
    x_present();
    x_fence();
    x_flush();
}

/* ====================================================================
 * running commands
 * ==================================================================== */

static void rescan(void)
{
    panel_load(&pan[0]);
    panel_load(&pan[1]);
    panel_goto(&pan[0], pan[0].cur);
    panel_goto(&pan[1], pan[1].cur);
}

/* Programs whose tabs were closed, to be waited for when they've gone. */
static pid_t closed_pids[32];
static int nclosed;

static void reap_closed(void)
{
    for (int i = 0; i < nclosed; i++)
        if (waitpid(closed_pids[i], NULL, WNOHANG) != 0)
            closed_pids[i--] = closed_pids[--nclosed];
}

static int tab_index(const struct tab *t)
{
    for (int i = 0; i < ntabs; i++)
        if (tabs[i] == t)
            return i;
    return -1;
}

/* Make t's panels the live ones (pan[], act and the command line). */
static void tab_make_live(struct tab *t)
{
    if (t == live)
        return;
    if (live) {
        live->pan[0] = pan[0];
        live->pan[1] = pan[1];
        live->act = act;
        live->cmdpos = cmdpos;
        memcpy(live->cmdline, cmdline, sizeof cmdline);
    }
    pan[0] = t->pan[0];
    pan[1] = t->pan[1];
    act = t->act;
    cmdpos = t->cmdpos;
    memcpy(cmdline, t->cmdline, sizeof cmdline);
    live = t;
    rescan();                           /* what changed while it was away */
}

static void tab_show(int i)
{
    if (i < 0 || i >= ntabs)
        return;
    cur_tab = i;
    if (CUR->kind == TAB_FILES)
        tab_make_live(CUR);
    relayout();
    job_dirty = 1;
}

/* Close tab i: a command still running in it is hung up on. The last
 * files tab stays. */
static void tab_close(int i)
{
    struct tab *t = tabs[i], *to = NULL;
    int nfiles = 0, was_cur = i == cur_tab;

    for (int k = 0; k < ntabs; k++)
        nfiles += tabs[k]->kind == TAB_FILES;
    if (t->kind == TAB_FILES) {
        if (nfiles < 2)
            return;
        for (int k = 0; k < ntabs; k++)
            if (tabs[k]->from == t)
                tabs[k]->from = NULL;
    } else {
        term_free(&t->t);
        if (!t->t.reaped && t->t.pid > 0 && !term_reap(&t->t) &&
            nclosed < (int)(sizeof closed_pids / sizeof closed_pids[0]))
            closed_pids[nclosed++] = t->t.pid;
        to = t->from;
    }
    memmove(tabs + i, tabs + i + 1, (size_t)(ntabs - i - 1) * sizeof *tabs);
    ntabs--;
    if (t == live) {    /* the nearest files tab to the left (or right) goes live */
        int f = -1;

        panel_free(&pan[0]);
        panel_free(&pan[1]);
        live = NULL;
        for (int k = 0; k < ntabs; k++)
            if (tabs[k]->kind == TAB_FILES && (f < 0 || k < i))
                f = k;
        tab_make_live(tabs[f]);
    } else if (t->kind == TAB_FILES) {
        panel_free(&t->pan[0]);
        panel_free(&t->pan[1]);
    }
    free(t);
    tab_hover = -1;
    if (was_cur) {
        int k = to ? tab_index(to) : -1;

        tab_show(k >= 0 ? k : i > 0 ? i - 1 : 0);
    } else {
        if (cur_tab > i)
            cur_tab--;
        job_dirty = 1;
    }
}

static struct tab *tab_new(int kind)
{
    struct tab *t;

    if (ntabs == MAX_TABS)              /* full: a finished command makes room */
        for (int i = 0; i < ntabs; i++)
            if (tabs[i]->kind == TAB_TERM && !tabs[i]->running && i != cur_tab) {
                tab_close(i);
                break;
            }
    if (ntabs == MAX_TABS || (t = calloc(1, sizeof *t)) == NULL) {
        say("too many tabs");
        return NULL;
    }
    t->kind = kind;
    tabs[ntabs++] = t;
    return t;
}

/* A new files tab, where this one is. */
static void tab_new_files(void)
{
    struct tab *t = tab_new(TAB_FILES);

    if (!t)
        return;
    for (int i = 0; i < 2; i++) {
        panel_init(&t->pan[i], pan[i].path);
        t->pan[i].sort = pan[i].sort;
        t->pan[i].reverse = pan[i].reverse;
    }
    t->act = act;
    tab_show(ntabs - 1);
}

/* Run script in a new terminal tab. With show, it's shown now; otherwise
 * when it first writes something, if that's soon and the files tab it
 * came from is still the one shown. (So an X program, or a command with
 * nothing to say, never takes the panels away.) */
static void job_start(const char *dir, const char *script, const char *title, int show)
{
    char *argv[] = { "/bin/sh", "-c", (char *)script, NULL };
    struct tab *t = tab_new(TAB_TERM);

    if (!t)
        return;
    t->t = term_proto;
    t->from = live;
    t->started = now_ms();
    snprintf(t->title, sizeof t->title, "%s", title);
    term_place(&t->t, b_out_text.x, b_out_text.y, b_out_text.w, b_out_text.h);
    if (term_start(&t->t, dir, argv) != 0) {
        say("can't start it: %s", strerror(errno));
        tab_close(ntabs - 1);
        relayout();                     /* (starting cleared its area) */
        return;
    }
    t->running = 1;
    if (show)
        tab_show(ntabs - 1);
    else
        relayout();
    job_dirty = 1;
}

static void job_finish(struct tab *t)
{
    /* (if it isn't reaped yet, something it started still runs; it is
     * waited for when the next command starts) */
    t->status = term_reap(&t->t) ? t->t.status : 0;
    t->running = 0;
    rescan();
    /* An X program, a command with nothing to say, or a full-screen one
     * (vi, mc) that ended well: its tab goes. */
    if (WIFEXITED(t->status) && WEXITSTATUS(t->status) == 0 && term_blank(&t->t))
        tab_close(tab_index(t));
    job_dirty = 1;
}

/* Read what the commands wrote; wait up to ms for X or a command. */
static void job_poll(int ms)
{
    struct pollfd p[1 + MAX_TABS];
    struct tab *who[1 + MAX_TABS];
    int n = 1;

    x_flush();
    if (x_pending())
        return;
    p[0].fd = x_fd();
    p[0].events = POLLIN;
    p[0].revents = 0;
    for (int i = 0; i < ntabs; i++)
        if (tabs[i]->kind == TAB_TERM && tabs[i]->running) {
            p[n].fd = term_fd(&tabs[i]->t);
            p[n].events = POLLIN;
            p[n].revents = 0;
            who[n++] = tabs[i];
        }
    if (resize_pending && ms > 50)
        ms = 50;
    if (poll(p, (nfds_t)n, ms < 0 ? -1 : ms) <= 0)
        return;
    for (int k = 1; k < n; k++) {
        struct tab *t = who[k];

        if (!p[k].revents)
            continue;
        if (term_pump(&t->t)) {
            if (t == CUR)
                job_dirty = 1;
            if (t->t.running && !t->spoke) {
                t->spoke = 1;
                if (CUR == t->from && !overlays && now_ms() - t->started < 1500)
                    tab_show(tab_index(t));
            }
        }
        if (!t->t.running)
            job_finish(t);
    }
}

/* The user menu's commands run like typed ones. */
void run_script(const char *dir, const char *script)
{
    char title[512];
    const char *nl = strchr(script, '\n');

    snprintf(title, sizeof title, "%.*s%s", nl ? (int)(nl - script) : (int)strlen(script), script,
             nl && nl[1] ? " ..." : "");
    job_start(dir, script, title, 0);
}

/* ====================================================================
 * actions
 * ==================================================================== */

static void view_file(const char *path);

static int is_program(const char *path)
{
    unsigned char h[4];
    int fd = open(path, O_RDONLY), ok = 0;

    if (fd >= 0) {
        ok = read(fd, h, 4) == 4 && ((h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F') ||
                                     (h[0] == '#' && h[1] == '!'));
        close(fd);
    }
    return ok;
}

static void go(int pi, const char *dir)
{
    char old[PATH_MAX];

    snprintf(old, sizeof old, "%s", pan[pi].path);
    if (panel_chdir(&pan[pi], dir) != 0) {
        dlg_message("Error", 1, "Cannot change to\n\"%s\"\n%s", dir, strerror(errno));
        return;
    }
    snprintf(prev_dir, sizeof prev_dir, "%s", old);
    panel_goto(&pan[pi], pan[pi].cur);
}

static void open_entry(int pi)
{
    struct panel *p = &pan[pi];
    struct entry *e = panel_current(p);
    char full[PATH_MAX];

    if (!e)
        return;
    if (entry_is_dir(e)) {
        go(pi, e->name);
        return;
    }
    if (path_join(full, sizeof full, p->path, e->name) != 0)
        return;
    if ((e->flags & E_EXEC) && is_program(full)) {
        char cmd[PATH_MAX + 8] = "./";

        quote_onto(cmd, sizeof cmd, e->name);
        job_start(p->path, cmd, cmd, 0);
        return;
    }
    view_file(full);
}

static void edit_current(void)
{
    struct entry *e = panel_current(&pan[act]);
    const char *ed = getenv("EDITOR");
    char cmd[PATH_MAX + 64];

    if (!e || entry_is_dir(e))
        return;
    snprintf(cmd, sizeof cmd, "%s ", ed && *ed ? ed : "vi");
    quote_onto(cmd, sizeof cmd, e->name);
    job_start(pan[act].path, cmd, cmd, 1);
}

static void do_cd(const char *arg)
{
    char dir[PATH_MAX];
    const char *home = getenv("HOME");

    while (*arg == ' ')
        arg++;
    if (!*arg || strcmp(arg, "~") == 0)
        snprintf(dir, sizeof dir, "%s", home && *home ? home : "/");
    else if (strcmp(arg, "-") == 0)
        snprintf(dir, sizeof dir, "%s", prev_dir[0] ? prev_dir : pan[act].path);
    else if (arg[0] == '~' && arg[1] == '/')
        snprintf(dir, sizeof dir, "%s%s", home ? home : "", arg + 1);
    else
        snprintf(dir, sizeof dir, "%s", arg);
    {
        size_t n = strlen(dir);

        while (n > 1 && dir[n - 1] == ' ')
            dir[--n] = 0;
    }
    go(act, dir);
}

static void run_cmdline(void)
{
    char *s = cmdline;

    while (*s == ' ')
        s++;
    if (!*s)
        return;
    if (strncmp(s, "cd", 2) == 0 && (s[2] == 0 || s[2] == ' '))
        do_cd(s + 2);
    else
        job_start(pan[act].path, s, s, 0);
    cmdline[0] = 0;
    cmdpos = 0;
}

static void cmd_insert(const char *s)
{
    size_t n = strlen(s), len = strlen(cmdline);

    if (len + n + 1 >= sizeof cmdline)
        return;
    memmove(cmdline + cmdpos + n, cmdline + cmdpos, len - (size_t)cmdpos + 1);
    memcpy(cmdline + cmdpos, s, n);
    cmdpos += (int)n;
}

static void mark_pattern(int on)
{
    static char pat[256] = "*";

    if (dlg_input_box(on ? "Select" : "Unselect", on ? "Mark the files matching:"
                                                     : "Unmark the files matching:",
                      pat, sizeof pat, 40))
        panel_mark_glob(&pan[act], pat, on);
}

static void select_all(int on)
{
    struct panel *p = &pan[act];

    for (int i = 0; i < p->n; i++)
        if (!(p->e[i].flags & E_UPDIR))
            panel_mark(p, i, on);
}

static void after_op(int changed)
{
    if (changed)
        rescan();
}

/* ====================================================================
 * the right-click menu
 * ==================================================================== */

enum {
    CMD_OPEN = 1, CMD_VIEW, CMD_EDIT, CMD_COPY, CMD_MOVE, CMD_DELETE, CMD_MKDIR, CMD_MARK,
    CMD_SELECT, CMD_UNSELECT, CMD_ALL, CMD_NONE, CMD_INVERT, CMD_HIDDEN, CMD_RESCAN, CMD_SWAP,
    CMD_OUTPUT, CMD_NAME, CMD_PATH, CMD_TERMINAL, CMD_NEWTAB, CMD_USER = 100
};

static void additem(pitem *it, int *n, const char *text, const char *side, int cmd, int on)
{
    if (*n >= MAXPITEMS)
        return;
    snprintf(it[*n].text, sizeof it[*n].text, "%s", text);
    snprintf(it[*n].side, sizeof it[*n].side, "%s", side ? side : "");
    it[*n].cmd = cmd;
    it[*n].enabled = on;
    (*n)++;
}

static void command(int cmd, struct usermenu *um);

static void context_menu(int x, int y, int on_entry)
{
    struct panel *p = &pan[act];
    struct entry *e = on_entry ? panel_current(p) : NULL;
    pitem it[MAXPITEMS];
    int n = 0, cmd, sel = p->nmarked > 0 || (e && !(e->flags & E_UPDIR));
    char title[300], t[256];
    struct usermenu um;
    int nu;

    if (p->nmarked > 0)
        snprintf(title, sizeof title, "%d marked", p->nmarked);
    else if (e)
        to_latin1(title, sizeof title, e->name);
    else
        to_latin1(title, sizeof title, path_base(p->path));
    if (e) {
        additem(it, &n, entry_is_dir(e) ? "Open" : "View", entry_is_dir(e) ? "Enter" : "F3",
                entry_is_dir(e) ? CMD_OPEN : CMD_VIEW, 1);
        if (!entry_is_dir(e))
            additem(it, &n, "Edit", "F4", CMD_EDIT, 1);
    }
    snprintf(t, sizeof t, "Copy to other panel");
    additem(it, &n, t, "F5", CMD_COPY, sel);
    additem(it, &n, "Move or rename", "F6", CMD_MOVE, sel);
    additem(it, &n, "Delete", "F8", CMD_DELETE, sel);
    additem(it, &n, "", NULL, 0, 0);
    if (e && !(e->flags & E_UPDIR))
        additem(it, &n, e->flags & E_MARKED ? "Unmark" : "Mark", "Ins", CMD_MARK, 1);
    additem(it, &n, "Select all", NULL, CMD_ALL, 1);
    if (p->nmarked)
        additem(it, &n, "Select none", NULL, CMD_NONE, 1);
    additem(it, &n, "Select by pattern...", "+", CMD_SELECT, 1);
    additem(it, &n, "Invert selection", "*", CMD_INVERT, 1);
    additem(it, &n, "", NULL, 0, 0);
    additem(it, &n, "New folder...", "F7", CMD_MKDIR, 1);
    additem(it, &n, "Terminal here", "C-n", CMD_TERMINAL, 1);
    if (e && !(e->flags & E_UPDIR))
        additem(it, &n, "Name to command line", NULL, CMD_NAME, 1);
    additem(it, &n, opt_show_hidden ? "Hide dot files" : "Show dot files", "M-.", CMD_HIDDEN, 1);
    additem(it, &n, "Rescan", "C-r", CMD_RESCAN, 1);

    nu = usermenu_open(&um, p, &pan[!act]);
    if (nu > 0) {
        additem(it, &n, "", NULL, 0, 0);
        for (int i = 0; i < nu && n < MAXPITEMS; i++) {
            const char *l = um.labels[i];
            char hk[2] = { um.hotkeys[i], 0 };

            /* "c       Compile with tcc": the title follows the hotkey */
            if (*l && l[1] && isspace((unsigned char)l[1]))
                l++;
            while (isspace((unsigned char)*l))
                l++;
            to_latin1(t, sizeof t, l);
            additem(it, &n, t, hk, CMD_USER + i, 1);
        }
    }
    present();
    cmd = popup(x, y, title, it, n);
    if (cmd)
        command(cmd, &um);
    usermenu_close(&um);
}

static void command(int cmd, struct usermenu *um)
{
    struct panel *p = &pan[act], *o = &pan[!act];
    struct entry *e = panel_current(p);

    switch (cmd) {
    case CMD_OPEN: open_entry(act); break;
    case CMD_VIEW:
        if (e && !entry_is_dir(e)) {
            char full[PATH_MAX];

            if (path_join(full, sizeof full, p->path, e->name) == 0)
                view_file(full);
        }
        break;
    case CMD_EDIT: edit_current(); break;
    case CMD_COPY: after_op(fop_copy(p, o, 0)); break;
    case CMD_MOVE: after_op(fop_copy(p, o, 1)); break;
    case CMD_DELETE: after_op(fop_delete(p)); break;
    case CMD_MKDIR: after_op(fop_mkdir(p)); break;
    case CMD_MARK:
        if (e && !(e->flags & E_UPDIR))
            panel_mark(p, p->cur, !(e->flags & E_MARKED));
        break;
    case CMD_SELECT: mark_pattern(1); break;
    case CMD_UNSELECT: mark_pattern(0); break;
    case CMD_ALL: select_all(1); break;
    case CMD_NONE: panel_unmark_all(p); break;
    case CMD_INVERT: panel_invert(p); break;
    case CMD_HIDDEN:
        opt_show_hidden = !opt_show_hidden;
        rescan();
        break;
    case CMD_RESCAN: rescan(); break;
    case CMD_SWAP: {
        struct panel t = pan[0];

        pan[0] = pan[1];
        pan[1] = t;
        relayout();
        break;
    }
    case CMD_OUTPUT:                    /* the latest terminal tab */
        for (int i = ntabs - 1; i >= 0; i--)
            if (tabs[i]->kind == TAB_TERM) {
                tab_show(i);
                break;
            }
        break;
    case CMD_TERMINAL:
        job_start(pan[act].path, "exec ${SHELL:-/bin/sh} -l", "sh", 1);
        break;
    case CMD_NEWTAB: tab_new_files(); break;
    case CMD_NAME:
        if (e) {
            char q[PATH_MAX + 8] = "";

            quote_onto(q, sizeof q, e->name);
            strcat(q, " ");
            cmd_insert(q);
        }
        break;
    default:
        if (cmd >= CMD_USER && um)
            usermenu_exec(um, cmd - CMD_USER);
    }
}

/* ====================================================================
 * the viewer
 * ==================================================================== */

static void view_file(const char *path)
{
    static struct tview v;
    struct stat st;
    int fd, done = 0, rows;
    box b, bt;
    char key[KEYLEN], tkey[KEYLEN] = "", search[128] = "", t[PATH_MAX];
    size_t want;
    const size_t cap = 768 * 1024;
    int truncated = 0;

    fd = open(path, O_RDONLY);
    if (fd < 0 || fstat(fd, &st) != 0) {
        dlg_message("Error", 1, "Cannot open\n\"%s\"\n%s", path, strerror(errno));
        if (fd >= 0)
            close(fd);
        return;
    }
    want = st.st_size > (off_t)cap ? cap : (size_t)st.st_size;
    truncated = st.st_size > (off_t)cap;
    tv_clear(&v);
    v.follow = 0;
    v.hex = 0;
    if (want + 1 > v.cap) {
        char *nb = realloc(v.buf, want + 1);

        if (!nb) {
            close(fd);
            dlg_message("Error", 1, "Not enough memory to view\n\"%s\"", path);
            return;
        }
        v.buf = nb;
        v.cap = want + 1;
    }
    while (v.len < want) {
        ssize_t r = read(fd, v.buf + v.len, want - v.len);

        if (r <= 0)
            break;
        v.len += (size_t)r;
    }
    close(fd);
    {   /* binary? then hex */
        size_t zeros = 0;

        for (size_t i = 0; i < v.len && i < 4096; i++)
            zeros += v.buf[i] == 0;
        v.hex = zeros > 0;
    }

    overlays++;                         /* the panels stay as they are */
    for (;;) {
        struct x_event e;
        int k;

        b = (box){ M, M + head_h() + G, x_width - 2 * M, x_height - 2 * M - head_h() - G };
        bt = (box){ b.x + P, b.y + P + font_h(f_bold) + 10, b.w - 2 * P - 8,
                    b.h - 2 * P - font_h(f_bold) - 10 };
        rows = tv_rows(bt);
        if (!tkey[0]) {
            x_color(c_bg);
            x_fill_rect(0, M + head_h(), x_width, x_height - M - head_h());
            x_color(c_panel);
            round_rect(b.x, b.y, b.w, b.h, RADIUS);
            tv_forget(&v);
        }
        tv_draw(&v, bt, c_panel);
        {
            int total = v.hex ? (int)((v.len + 15) / 16) : v.nlines;
            int pct = total > rows ? (int)((long)(v.top + rows) * 100 / total) : 100;

            if (pct > 100)
                pct = 100;
            snprintf(key, sizeof key, "%d|%d|%d|%d|%d", v.top, total, pct, v.hex, truncated);
            if (strcmp(key, tkey) != 0) {
                char right[96];

                strcpy(tkey, key);
                x_color(c_panel);
                x_fill_rect(b.x + P, b.y + P - 2, b.w - 2 * P, font_h(f_bold) + 4);
                x_color(px(ACC_BLUE));
                round_rect(b.x + P, b.y + P + 2, 4, x_font_ascent(f_bold) - 1, 2);
                snprintf(right, sizeof right, "%s%s   line %d of %d   %d%%",
                         truncated ? "first 768K   " : "", v.hex ? "hex" : "text",
                         v.top + 1, total, pct);
                to_latin1(t, sizeof t, path);
                text_at(f_bold, b.x + P + 10, b.y + P, b.w - 2 * P - 10 - x_text_width(f_text, right)
                        - 20, 0, TRIM_START, c_text, t);
                text_at(f_text, b.x + P, b.y + P, b.w - 2 * P, 1, TRIM_END, c_dim, right);
                text_at(f_text, b.x + P, b.y + b.h - P - font_h(f_text) + 4, b.w - 2 * P, 2,
                        TRIM_END, c_faint, "Esc or q close     F4 hex/text     / search, n next     wheel or arrows scroll");
            }
        }
        x_present();
        x_sync();
        if (done)
            break;
        if (!next_event(&e, -1)) {
            if (resize_pending && now_ms() >= resize_at) {
                resize_pending = 0;
                tkey[0] = 0;
            }
            continue;
        }
        if (resize_pending && now_ms() >= resize_at) {
            resize_pending = 0;
            tkey[0] = 0;
        }
        if (e.type == XE_CLOSE)
            break;
        if (e.type == XE_BUTTON_DOWN && (e.button == 4 || e.button == 5)) {
            v.top += e.button == 4 ? -3 : 3;
            continue;
        }
        if (e.type != XE_KEY_DOWN)
            continue;
        k = xkey(&e);
        switch (k) {
        case K_ESC: case 'q': case K_F(3): case K_F(10): done = 1; break;
        case K_UP: v.top--; break;
        case K_DOWN: case K_ENTER: v.top++; break;
        case K_PGUP: case 'b': v.top -= rows - 1; break;
        case K_PGDN: case ' ': v.top += rows - 1; break;
        case K_HOME: case 'g': v.top = 0; break;
        case K_END: case 'G': v.top = 1 << 30; break;
        case K_F(4): case 'h':
            v.hex = !v.hex;
            v.top = 0;
            v.hl = -1;
            tv_reindex(&v);
            tv_forget(&v);
            break;
        case '/': case K_F(7):
            overlays--;
            if (dlg_input_box("Search", "Find (ignoring case):", search, sizeof search, 40)) {
                int at = tv_search(&v, search, v.top - 1);

                if (at >= 0) {
                    v.hl = at;
                    v.top = at - rows / 3;
                } else {
                    say("not found: %s", search);
                }
            }
            overlays++;
            break;
        case 'n':
            if (search[0]) {
                int at = tv_search(&v, search, v.hl >= 0 ? v.hl : v.top - 1);

                if (at >= 0) {
                    v.hl = at;
                    if (at < v.top || at >= v.top + rows)
                        v.top = at - rows / 3;
                }
            }
            break;
        }
    }
    overlays--;
    free(v.buf);
    free(v.line);
    memset(&v, 0, sizeof v);
    relayout();
}

/* ====================================================================
 * mouse
 * ==================================================================== */

static int in_box(box b, int x, int y)
{
    return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

static int panel_at(int x, int y)
{
    for (int i = 0; i < 2; i++)
        if (in_box(geo[i].b, x, y))
            return i;
    return -1;
}

/* The entry under (x, y) in panel pi, or -1. */
static int row_at(int pi, int x, int y)
{
    struct pgeo *g = &geo[pi];
    int r;

    if (!in_box(g->b, x, y) || y < g->list_y || y >= g->list_y + g->rows * g->row_h)
        return -1;
    r = (y - g->list_y) / g->row_h + pan[pi].top;
    return r < pan[pi].n ? r : -1;
}

static void drag_overlay_clear(void)
{
    if (!drag.ghost.save && !drag.target.save)
        return;
    under_restore(&drag.ghost);
    under_restore(&drag.target);
}

/* Where would the files land if dropped at (x, y)? */
static void drag_target(int x, int y, int mods)
{
    int pi = panel_at(x, y), r = pi >= 0 ? row_at(pi, x, y) : -1;
    struct panel *src = &pan[drag.panel];
    int n = src->nmarked ? src->nmarked : 1, move = 0;
    char where[PATH_MAX], name[300];

    drag.target_kind = 0;
    if (in_box(b_cmd, x, y)) {
        drag.target_kind = 3;
        snprintf(drag.label, sizeof drag.label, "Type %s", n > 1 ? "their names" : "its name");
        return;
    }
    if (pi < 0) {
        snprintf(drag.label, sizeof drag.label, "%d file%s", n, n == 1 ? "" : "s");
        return;
    }
    if (r >= 0 && entry_is_dir(&pan[pi].e[r]) && !(pan[pi].e[r].flags & E_MARKED) &&
        !(pi == drag.panel && r == drag.row)) {
        drag.target_kind = 2;
        drag.target_panel = pi;
        drag.target_row = r;
        if (path_join(where, sizeof where, pan[pi].path, pan[pi].e[r].name) != 0)
            return;
        path_normalize(where);
        move = pi == drag.panel ? !(mods & X_MOD_CTRL) : (mods & X_MOD_SHIFT) != 0;
    } else if (pi != drag.panel) {
        drag.target_kind = 1;
        drag.target_panel = pi;
        snprintf(where, sizeof where, "%s", pan[pi].path);
        move = (mods & X_MOD_SHIFT) != 0;
    } else {
        snprintf(drag.label, sizeof drag.label, "%d file%s", n, n == 1 ? "" : "s");
        return;
    }
    to_latin1(name, sizeof name, path_base(where));
    snprintf(drag.label, sizeof drag.label, "%s %d file%s to %s", move ? "Move" : "Copy", n,
             n == 1 ? "" : "s", where[1] ? name : "/");
    drag.mode = 0;
    drag.mark_to = move;            /* reused: the drop moves */
}

/* Show the label by the pointer and the target's outline. */
static void drag_show(int x, int y)
{
    int w = x_text_width(f_bold, drag.label) + 24, h = font_h(f_bold) + 10;
    box lb = { x + 14, y + 10, w, h };

    drag_overlay_clear();
    if (drag.target_kind == 1) {
        box pb = geo[drag.target_panel].b;

        under_save(&drag.target, (box){ pb.x - 2, pb.y - 2, pb.w + 4, pb.h + 4 });
        x_color(px(ACC_GREEN));
        x_line_width(2);
        x_rect(pb.x, pb.y, pb.w - 1, pb.h - 1);
        x_line_width(1);
    }
    if (lb.x + lb.w > x_width)
        lb.x = x - 14 - lb.w;
    if (lb.y + lb.h > x_height)
        lb.y = y - 10 - lb.h;
    under_save(&drag.ghost, lb);
    x_color(drag.target_kind ? px(mix(RGB_PANEL, ACC_GREEN, 500)) : c_track);
    round_rect(lb.x, lb.y, lb.w, lb.h, 6);
    text_at(f_bold, lb.x, lb.y + 5, lb.w, 2, TRIM_END, c_text, drag.label);
}

static void drop(int mods)
{
    struct panel *src = &pan[drag.panel];
    int move = drag.mark_to;

    (void)mods;
    if (drag.target_kind == 3) {
        int *idx, n = panel_selection(src, &idx);

        for (int i = 0; i < n; i++) {
            char full[PATH_MAX], q[PATH_MAX + 8] = "";

            if (path_join(full, sizeof full, src->path, src->e[idx[i]].name) != 0)
                continue;
            quote_onto(q, sizeof q, drag.panel == act ? src->e[idx[i]].name : full);
            strcat(q, " ");
            cmd_insert(q);
        }
        free(idx);
        return;
    }
    act = drag.panel;
    if (drag.target_kind == 1) {
        after_op(fop_copy(src, &pan[drag.target_panel], move));
    } else if (drag.target_kind == 2) {
        static struct panel to;

        memset(&to, 0, sizeof to);
        if (path_join(to.path, sizeof to.path, pan[drag.target_panel].path,
                      pan[drag.target_panel].e[drag.target_row].name) == 0) {
            path_normalize(to.path);
            after_op(fop_copy(src, &to, move));
        }
    }
}

/* Scroll a terminal tab's history by d lines (down is positive). */
static void out_scroll(int d)
{
    if (ON_TERM)
        term_scroll(&CUR->t, -d);
}

static void settings_menu(void)
{
    pitem it[4];
    int n = 0, cmd;

    additem(it, &n, "Show dot files", opt_show_hidden ? "on" : "off", CMD_HIDDEN, 1);
    present();
    cmd = popup(b_gear.x + b_gear.w, b_gear.y + b_gear.h + 4, NULL, it, n);
    if (cmd)
        command(cmd, NULL);
}

static int tab_at(int x, int y)
{
    for (int i = 0; i < ntabs; i++)
        if (in_box(tb_box[i], x, y))
            return i;
    return -1;
}

/* A click on the tab bar: Returns whether it was. */
static int click_tabs(const struct x_event *e)
{
    int i = tab_at(e->x, e->y);

    if (e->y >= M + head_h() + 2)
        return 0;
    if (e->button == 4 || e->button == 5) {     /* the wheel goes through them */
        tab_show((cur_tab + (e->button == 4 ? ntabs - 1 : 1)) % ntabs);
    } else if (i >= 0 && (e->button == 2 ||
                          (e->button == 1 && tb_has_close[i] && in_box(tb_close[i], e->x, e->y)))) {
        tab_close(i);
    } else if (i >= 0 && e->button == 1) {
        tab_show(i);
    } else if (e->button == 1 && in_box(b_plus, e->x, e->y)) {
        tab_new_files();
    } else if (e->button == 1 && in_box(b_gear, e->x, e->y)) {
        settings_menu();
    }
    return 1;
}

static long long last_click;
static int last_click_panel = -1, last_click_row = -1;
static int bar_drag = -1, bar_grab;

static void click(const struct x_event *e)
{
    int pi = panel_at(e->x, e->y);

    if (click_tabs(e))
        return;
    if (ON_TERM) {
        if (e->button >= 4 || (CUR->running && in_box(b_out_text, e->x, e->y)))
            term_mouse(&CUR->t, e);     /* history, or the program's mouse */
        return;
    }
    if (e->button == 4 || e->button == 5) {
        if (pi >= 0) {
            struct panel *p = &pan[pi];
            int rows = geo[pi].rows, d = e->button == 4 ? -3 : 3;

            p->top += d;
            if (p->top > p->n - rows)
                p->top = p->n - rows;
            if (p->top < 0)
                p->top = 0;
            if (p->cur < p->top)
                p->cur = p->top;
            if (p->cur >= p->top + rows)
                p->cur = p->top + rows - 1;
        }
        return;
    }
    if (pi < 0)
        return;
    act = pi;
    {
        struct pgeo *g = &geo[pi];
        struct panel *p = &pan[pi];
        int r = row_at(pi, e->x, e->y);

        /* the path: go to the part clicked */
        if (e->button == 1 && e->y >= g->crumb_y && e->y < g->crumb_y + font_h(f_bold) + 4) {
            for (int i = 0; i < g->ncrumb; i++)
                if (e->x >= g->crumb_x[i] && e->x < g->crumb_x[i] + g->crumb_w[i]) {
                    char dir[PATH_MAX];

                    snprintf(dir, sizeof dir, "%.*s", g->crumb_end[i], p->path);
                    if (!dir[0])
                        strcpy(dir, "/");
                    go(pi, dir);
                    break;
                }
            return;
        }
        /* a column heading: sort */
        if (e->button == 1 && e->y >= g->head_y && e->y < g->list_y - 3) {
            int key = e->x >= g->date_x && g->date_w ? SORT_MTIME
                    : e->x >= g->size_x && g->size_w ? SORT_SIZE : SORT_NAME;

            if (p->sort == key)
                p->reverse = !p->reverse;
            else
                p->sort = key, p->reverse = key != SORT_NAME;
            panel_load(p);
            return;
        }
        /* the scroll bar */
        if (e->button == 1 && e->x >= g->b.x + g->b.w - P - 2 && e->y >= g->list_y &&
            e->y < g->list_y + g->rows * g->row_h && p->n > g->rows) {
            bar_drag = pi;
            bar_grab = e->y;
            return;
        }
        if (e->button == 3) {
            if (r >= 0)
                panel_goto(p, r);
            context_menu(e->x, e->y, r >= 0);
            return;
        }
        if (e->button != 1 || r < 0)
            return;
        if (e->mods & X_MOD_CTRL) {             /* mark one */
            if (!(p->e[r].flags & E_UPDIR))
                panel_mark(p, r, !(p->e[r].flags & E_MARKED));
            panel_goto(p, r);
            shift_anchor[pi] = r;
            memset(&drag, 0, sizeof drag);
            drag.on = 1;
            drag.mode = 1;
            drag.panel = pi;
            drag.row = r;
            drag.mark_to = (p->e[r].flags & E_MARKED) != 0;
            drag.last_row = r;
            drag.x0 = e->x;
            drag.y0 = e->y;
            return;
        }
        if (e->mods & X_MOD_SHIFT) {            /* mark a range */
            int a = shift_anchor[pi] >= 0 ? shift_anchor[pi] : p->cur;
            int lo = a < r ? a : r, hi = a < r ? r : a;

            for (int i = lo; i <= hi; i++)
                if (!(p->e[i].flags & E_UPDIR))
                    panel_mark(p, i, 1);
            panel_goto(p, r);
            memset(&drag, 0, sizeof drag);
            drag.on = 1;
            drag.mode = 1;
            drag.panel = pi;
            drag.row = r;
            drag.mark_to = 1;
            drag.last_row = r;
            drag.x0 = e->x;
            drag.y0 = e->y;
            return;
        }
        if (now_ms() - last_click < DCLICK_MS && last_click_panel == pi && last_click_row == r) {
            last_click = 0;
            panel_goto(p, r);
            open_entry(pi);
            return;
        }
        last_click = now_ms();
        last_click_panel = pi;
        last_click_row = r;
        shift_anchor[pi] = r;
        panel_goto(p, r);
        memset(&drag, 0, sizeof drag);
        drag.on = 1;
        drag.panel = pi;
        drag.row = r;
        drag.x0 = e->x;
        drag.y0 = e->y;
    }
}

static int motion(const struct x_event *e)
{
    if (bar_drag >= 0) {
        struct pgeo *g = &geo[bar_drag];
        struct panel *p = &pan[bar_drag];
        int bh = g->rows * g->row_h, th = bh * g->rows / p->n;
        int range = p->n - g->rows, top;

        if (th < 16)
            th = 16;
        top = (int)((long)(e->y - g->list_y - th / 2) * range / (bh - th > 0 ? bh - th : 1));
        p->top = top < 0 ? 0 : top > range ? range : top;
        if (p->cur < p->top)
            p->cur = p->top;
        if (p->cur >= p->top + g->rows)
            p->cur = p->top + g->rows - 1;
        return 1;
    }
    if (!drag.on)
        return 0;
    if (!drag.active) {
        if (abs(e->x - drag.x0) < DRAG_PX && abs(e->y - drag.y0) < DRAG_PX)
            return 0;
        drag.active = 1;
        if (drag.mode == 0) {                   /* dragging files */
            struct panel *p = &pan[drag.panel];

            if (!(p->e[drag.row].flags & E_MARKED) && p->nmarked)
                panel_unmark_all(p);
            if (p->e[drag.row].flags & E_UPDIR) {
                drag.on = drag.active = 0;
                return 0;
            }
        }
    }
    if (drag.mode == 1) {                       /* marking the rows passed */
        int r = row_at(drag.panel, geo[drag.panel].b.x + P + 30, e->y);
        struct panel *p = &pan[drag.panel];

        if (r < 0 || r == drag.last_row)
            return 0;
        for (int i = drag.last_row < r ? drag.last_row : r;
             i <= (drag.last_row < r ? r : drag.last_row); i++)
            if (!(p->e[i].flags & E_UPDIR))
                panel_mark(p, i, drag.mark_to);
        drag.last_row = r;
        panel_goto(p, r);
        return 1;
    }
    drag.px = e->x;
    drag.py = e->y;
    drag_target(e->x, e->y, e->mods);
    return 1;                           /* present() moves the label */
}

static int release(const struct x_event *e)
{
    if (bar_drag >= 0) {
        bar_drag = -1;
        return 1;
    }
    if (!drag.on)
        return 0;
    if (drag.active && drag.mode == 0) {
        drag_overlay_clear();
        drag_target(e->x, e->y, e->mods);
        drag.active = 0;
        draw_all();
        x_present();
        if (drag.target_kind)
            drop(e->mods);
    }
    drag.on = drag.active = 0;
    drag.target_kind = 0;
    return 1;
}

/* ====================================================================
 * keys
 * ==================================================================== */

static int on_key(int k)
{
    struct panel *p = &pan[act];
    struct entry *e = panel_current(p);
    int typing = cmdline[0] != 0;

    if (ON_TERM) {                      /* finished (keys go to it while it runs) */
        int rows = CUR->t.started ? CUR->t.vt.rows : 10;

        switch (k) {
        case K_UP: out_scroll(-1); break;
        case K_DOWN: out_scroll(1); break;
        case K_PGUP: out_scroll(-(rows - 1)); break;
        case K_PGDN: out_scroll(rows - 1); break;
        case K_HOME: out_scroll(-(1 << 30)); break;
        case K_END: out_scroll(1 << 30); break;
        default: tab_close(cur_tab);
        }
        return 0;
    }

    switch (k) {
    case K_F(10): return 1;
    case K_TAB: act = !act; break;
    case K_UP: panel_move(p, -1); break;
    case K_DOWN: panel_move(p, 1); break;
    case K_PGUP: panel_move(p, -(geo[act].rows - 1)); break;
    case K_PGDN: panel_move(p, geo[act].rows - 1); break;
    case K_HOME: if (typing) cmdpos = 0; else panel_goto(p, 0); break;
    case K_END: if (typing) cmdpos = (int)strlen(cmdline); else panel_goto(p, p->n - 1); break;
    case K_LEFT:
        if (typing && cmdpos > 0)
            cmdpos--;
        break;
    case K_RIGHT:
        if (typing && cmdline[cmdpos])
            cmdpos++;
        break;
    case K_UP | K_ALT: case K_CTRL_PGUP: go(act, ".."); break;
    case K_CTRL_PGDN: if (e && entry_is_dir(e)) go(act, e->name); break;
    case K_ENTER:
        if (typing)
            run_cmdline();
        else
            open_entry(act);
        break;
    case K_ENTER | K_ALT:
        if (e) {
            char q[PATH_MAX + 8] = "";

            quote_onto(q, sizeof q, e->name);
            strcat(q, " ");
            cmd_insert(q);
        }
        break;
    case K_INS: case K_CTRL('t'):
        if (e && !(e->flags & E_UPDIR))
            panel_mark(p, p->cur, !(e->flags & E_MARKED));
        panel_move(p, 1);
        break;
    case K_F(2): {
        struct pgeo *g = &geo[act];

        context_menu(g->name_x + 40, g->list_y + (p->cur - p->top + 1) * g->row_h, 1);
        break;
    }
    case K_F(3): command(CMD_VIEW, NULL); break;
    case K_F(4): command(CMD_EDIT, NULL); break;
    case K_F(5): command(CMD_COPY, NULL); break;
    case K_F(6): command(CMD_MOVE, NULL); break;
    case K_F(7): command(CMD_MKDIR, NULL); break;
    case K_F(8): command(CMD_DELETE, NULL); break;
    case K_DEL:
        if (typing && cmdline[cmdpos]) {
            int q = cmdpos + 1;

            while (cmdline[q] && ((unsigned char)cmdline[q] & 0xC0) == 0x80)
                q++;
            memmove(cmdline + cmdpos, cmdline + q, strlen(cmdline + q) + 1);
        }
        break;
    case K_CTRL('o'): command(CMD_OUTPUT, NULL); break;
    case K_CTRL('r'): command(CMD_RESCAN, NULL); break;
    case K_CTRL('n'): command(CMD_TERMINAL, NULL); break;
    case K_CTRL('u'): command(CMD_SWAP, NULL); break;
    case K_CTRL('c'): cmdline[0] = 0, cmdpos = 0; break;
    case '.' | K_ALT: command(CMD_HIDDEN, NULL); break;
    case K_ESC:
        if (typing)
            cmdline[0] = 0, cmdpos = 0;
        break;
    case K_BS:
        if (cmdpos > 0) {
            int q = cmdpos;

            while (q > 0 && ((unsigned char)cmdline[--q] & 0xC0) == 0x80)
                ;
            memmove(cmdline + q, cmdline + cmdpos, strlen(cmdline + cmdpos) + 1);
            cmdpos = q;
        } else if (!typing) {
            go(act, "..");
        }
        break;
    default:
        if (!typing && (k == '+' || k == '-' || k == '*')) {
            if (k == '*')
                panel_invert(p);
            else
                mark_pattern(k == '+');
        } else if (k >= 32 && k < 256 && k != 127) {
            char u[3] = { (char)k, 0, 0 };

            if (k >= 128) {
                u[0] = (char)(0xC0 | (k >> 6));
                u[1] = (char)(0x80 | (k & 0x3F));
            }
            cmd_insert(u);
        }
    }
    return 0;
}

/* Shift with an arrow: mark while moving. */
static void shift_move(int d)
{
    struct panel *p = &pan[act];
    struct entry *e = panel_current(p);

    if (e && !(e->flags & E_UPDIR))
        panel_mark(p, p->cur, 1);
    panel_move(p, d);
    e = panel_current(p);
    if (e && !(e->flags & E_UPDIR))
        panel_mark(p, p->cur, 1);
}

int main(int argc, char **argv)
{
    char cwd[PATH_MAX];
    int quit = 0;

    signal(SIGPIPE, SIG_IGN);
    if (getcwd(cwd, sizeof cwd) == NULL)
        strcpy(cwd, "/");
    panel_init(&pan[0], argc > 1 ? argv[1] : cwd);
    panel_init(&pan[1], argc > 2 ? argv[2] : argc > 1 ? argv[1] : cwd);
    if (panel_load(&pan[0]) != 0)
        panel_chdir(&pan[0], "/");
    if (panel_load(&pan[1]) != 0)
        panel_chdir(&pan[1], "/");

    if (x_open(NULL) < 0)
        return 1;
    x_background(x_rgb(RGB_BG.r, RGB_BG.g, RGB_BG.b));
    x_window("xmc", 1100, 720);
    x_min_size(640, 420);
    setup_style();
    term_init(&term_proto, 13);
    term_proto.bg = c_panel;
    term_proto.fg = c_text;
    term_proto.cursor = px(ACC_BLUE);
    tabs[0] = calloc(1, sizeof *tabs[0]);   /* the first files tab: pan[] */
    if (!tabs[0])
        return 1;
    ntabs = 1;
    live = tabs[0];
    relayout();
    present();

    while (!quit) {
        struct x_event e;
        int dirty = 0, n = 0, got;

        wake_on_output = 1;             /* only while this loop waits */
        got = next_event(&e, resize_pending ? 30 : status_msg[0] ? 250 : 2000);
        wake_on_output = 0;
        if (got) do {
            switch (e.type) {
            case XE_CLOSE:
                quit = 1;
                break;
            case XE_KEY_DOWN: {
                int k;

                if ((e.key == X_KEY_TAB || e.key == X_KEY_BACKTAB) && (e.mods & X_MOD_CTRL)) {
                    int d = e.key == X_KEY_BACKTAB || (e.mods & X_MOD_SHIFT) ? ntabs - 1 : 1;

                    tab_show((cur_tab + d) % ntabs);        /* Ctrl-Tab: the next tab */
                    dirty = 1;
                    break;
                }
                if (CUR_RUNNING) {          /* a terminal: all keys to it */
                    term_key(&CUR->t, &e);
                    dirty = 1;
                    break;
                }
                k = xkey(&e);
                if (k == K_NONE)
                    break;
                dirty = 1;
                if ((e.mods & X_MOD_SHIFT) && !ON_TERM && (k == K_UP || k == K_DOWN)) {
                    shift_move(k == K_UP ? -1 : 1);
                    break;
                }
                quit = on_key(k);
                break;
            }
            case XE_BUTTON_DOWN:
                click(&e);
                dirty = 1;
                break;
            case XE_BUTTON_UP:
                if (CUR_RUNNING)
                    term_mouse(&CUR->t, &e);
                else if (e.button == 1 && !ON_TERM)
                    dirty |= release(&e);
                break;
            case XE_MOTION: {
                int h = drag.on || bar_drag >= 0 ? -1 : tab_at(e.x, e.y);

                if (h != tab_hover) {
                    tab_hover = h;
                    dirty = 1;
                }
                if (CUR_RUNNING)
                    term_mouse(&CUR->t, &e);
                else if (!ON_TERM)
                    dirty |= motion(&e);    /* only a drag changes anything */
                break;
            }
            }
        } while (!quit && ++n < 256 && next_event(&e, 0));
        if (resize_pending && now_ms() >= resize_at) {
            resize_pending = 0;
            relayout();
            dirty = 1;
        }
        if (status_msg[0] && now_ms() >= status_until) {
            status_msg[0] = 0;
            dirty = 1;
        }
        if (job_dirty) {
            job_dirty = 0;
            dirty = 1;
        }
        reap_closed();
        if (dirty)
            present();
    }
    for (int i = 0; i < ntabs; i++)
        if (tabs[i]->kind == TAB_TERM)
            term_close(&tabs[i]->t);
    x_close();
    return 0;
}
