/*
 * dialog.c - modal dialogs, input fields, buttons. See dialog.h.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "dialog.h"
#include "keys.h"
#include "screen.h"
#include "skin.h"
#include "util.h"

/* The colour set of a normal or an error dialog. */
struct dlg_colors {
    int normal, hot, focus, hotfocus, title;
};

static const struct dlg_colors colors_normal = {
    A_DLG, A_DLG_HOT, A_DLG_FOCUS, A_DLG_HOTFOCUS, A_DLG_TITLE
};
static const struct dlg_colors colors_error = {
    A_ERR, A_ERR_HOT, A_ERR_FOCUS, A_ERR_HOTFOCUS, A_ERR_TITLE
};
static const struct dlg_colors colors_menu = {
    A_MENU, A_MENU_HOT, A_MENU_HOTSEL, A_MENU_HOTSEL, A_MENU_HOT
};

static const struct dlg_colors *colors_of(const struct dialog *d)
{
    return d->scheme == DLG_ERROR ? &colors_error
         : d->scheme == DLG_MENU ? &colors_menu : &colors_normal;
}

static dlg_painter painter = app_draw;

dlg_painter dlg_set_painter(dlg_painter paint)
{
    dlg_painter old = painter;

    painter = paint != NULL ? paint : app_draw;
    return old;
}

/* Dialogs currently on screen, bottom first. */
#define STACK_MAX 8
static struct dialog *stack[STACK_MAX];
static int depth;

/* ---- building --------------------------------------------------------- */

void dlg_init(struct dialog *d, const char *title, int h, int w, int scheme)
{
    memset(d, 0, sizeof *d);
    d->title = title;
    d->h = h;
    d->w = w;
    d->scheme = scheme;
    d->focus = -1;
}

static struct widget *add(struct dialog *d, int type, int y, int x, int w)
{
    struct widget *wd;

    if (d->n >= DLG_MAX_WIDGETS)
        d->n = DLG_MAX_WIDGETS - 1;     /* overwrite rather than overflow */
    wd = &d->wd[d->n++];
    memset(wd, 0, sizeof *wd);
    wd->type = type;
    wd->y = y;
    wd->x = x;
    wd->w = w;
    return wd;
}

struct widget *dlg_label(struct dialog *d, int y, const char *text,
                         int center)
{
    struct widget *w = add(d, W_LABEL, y, 3, d->w - 6);

    w->text = text;
    w->flags = center ? WF_CENTER : 0;
    return w;
}

struct widget *dlg_input(struct dialog *d, int y, char *buf, int cap)
{
    struct widget *w = add(d, W_INPUT, y, 3, d->w - 6);

    w->buf = buf;
    w->cap = cap;
    w->pos = (int)strlen(buf);
    return w;
}

struct widget *dlg_gauge(struct dialog *d, int y)
{
    return add(d, W_GAUGE, y, 3, d->w - 6);
}

struct widget *dlg_list_box(struct dialog *d, int y, int h,
                            const char *const *items, const char *hotkeys,
                            int n)
{
    struct widget *w = add(d, W_LIST, y, 3, d->w - 6);

    w->h = h;
    w->items = items;
    w->hotkeys = hotkeys;
    w->cap = n;
    return w;
}

void dlg_hline(struct dialog *d, int y)
{
    add(d, W_HLINE, y, 1, d->w - 2);
}

/* Width of a caption on screen, without its '&' marker. */
static int caption_len(const char *s)
{
    return utf8_len(s) - (strchr(s, '&') != NULL);
}

static int button_width(const char *label, int is_default)
{
    return caption_len(label) + (is_default ? 6 : 4);
}

void dlg_buttons(struct dialog *d, int y, int gap, const char *const *labels,
                 int n, int def)
{
    int i, total = gap * (n - 1), x;

    for (i = 0; i < n; i++)
        total += button_width(labels[i], i == def);
    x = (d->w - total) / 2;
    for (i = 0; i < n; i++) {
        int bw = button_width(labels[i], i == def);
        struct widget *w = add(d, W_BUTTON, y, x, bw);

        w->text = labels[i];
        w->id = i;
        w->flags = i == def ? WF_DEFAULT : 0;
        x += bw + gap;
    }
}

/* ---- drawing ---------------------------------------------------------- */

/* Centre on screen, slightly above the middle as mc does. */
static void place(struct dialog *d)
{
    if (d->w > scr_cols)
        d->w = scr_cols;
    if (d->h > scr_rows)
        d->h = scr_rows;
    d->x = (scr_cols - d->w + 1) / 2;
    d->y = (scr_rows - d->h) / 2 - 2;
    if (d->y < 0)
        d->y = 0;
}

static int hotkey_of(const char *s)
{
    const char *amp = strchr(s, '&');

    return amp != NULL && amp[1] ? (unsigned char)amp[1] : 0;
}

/* Text with the letter after '&' in the hotkey colour. */
static int put_caption(int y, int x, const char *s, int attr, int hot)
{
    int is_hot = 0;

    while (*s) {
        unsigned c = utf8_next(&s);

        if (c == '&' && !is_hot) {
            is_hot = 1;
            continue;
        }
        scr_put(y, x++, c, is_hot == 1 ? hot : attr);
        if (is_hot == 1)
            is_hot = 2;
    }
    return x;
}

/* Columns occupied by bytes [from, to) of a UTF-8 string. */
static int cols_between(const char *s, int from, int to)
{
    int n = 0;

    for (; from < to; from++)
        if ((s[from] & 0xC0) != 0x80)
            n++;
    return n;
}

static void draw_input(const struct dialog *d, struct widget *w, int focused)
{
    int y = d->y + w->y, x = d->x + w->x;

    if (w->pos < w->scroll)
        w->scroll = w->pos;
    while (cols_between(w->buf, w->scroll, w->pos) >= w->w) {
        w->scroll++;
        while ((w->buf[w->scroll] & 0xC0) == 0x80)
            w->scroll++;
    }
    scr_text(y, x, w->w, w->buf + w->scroll,
             w->changed ? A_INPUT : A_INPUT_UNCH, T_LEFT);
    if (focused)
        scr_cursor(y, x + cols_between(w->buf, w->scroll, w->pos));
}

static void draw_gauge(const struct dialog *d, const struct widget *w,
                       const struct dlg_colors *c)
{
    int y = d->y + w->y, x = d->x + w->x, bw = w->w - 7, fill, i;
    char pct[8];

    if (bw < 1)
        return;
    fill = w->value * bw / 1000;
    scr_put(y, x, '[', c->normal);
    for (i = 0; i < bw; i++)
        scr_put(y, x + 1 + i, ' ', i < fill ? A_GAUGE : c->normal);
    scr_put(y, x + 1 + bw, ']', c->normal);
    sprintf(pct, "%3d%%", w->value / 10);
    scr_puts(y, x + bw + 3, pct, c->normal);
}

static void dlg_draw(struct dialog *d)
{
    const struct dlg_colors *c = colors_of(d);
    int i;

    scr_fill(d->y, d->x, d->h, d->w, ' ', c->normal);
    scr_box(d->y + 1, d->x + 1, d->h - 2, d->w - 2, c->normal);
    if (d->title != NULL) {
        int tl = utf8_len(d->title) + 2;
        int tx = d->x + (d->w - tl) / 2;

        scr_put(d->y + 1, tx, ' ', c->title);
        scr_puts(d->y + 1, tx + 1, d->title, c->title);
        scr_put(d->y + 1, tx + tl - 1, ' ', c->title);
    }

    for (i = 0; i < d->n; i++) {
        struct widget *w = &d->wd[i];
        int y = d->y + w->y, x = d->x + w->x, focused = i == d->focus;

        switch (w->type) {
        case W_LABEL:
            if (w->flags & WF_CENTER)   /* centred between the frame */
                scr_text(y, x - 1, w->w + 2, w->text, c->normal, T_CENTER);
            else
                scr_text(y, x, w->w, w->text, c->normal, T_LEFT);
            break;
        case W_INPUT:
            draw_input(d, w, focused);
            break;
        case W_HLINE:
            scr_put(y, x, BOX_LT, c->normal);
            scr_hline(y, x + 1, w->w - 2, BOX_H, c->normal);
            scr_put(y, x + w->w - 1, BOX_RT, c->normal);
            break;
        case W_GAUGE:
            draw_gauge(d, w, c);
            break;
        case W_LIST: {
            int r;

            if (w->pos < w->scroll)
                w->scroll = w->pos;
            if (w->pos >= w->scroll + w->h)
                w->scroll = w->pos - w->h + 1;
            for (r = 0; r < w->h; r++) {
                int idx = w->scroll + r;

                scr_text(y + r, x, w->w, idx < w->cap ? w->items[idx] : "",
                         idx == w->pos ? c->focus : c->normal, T_LEFT);
            }
            break;
        }
        case W_BUTTON: {
            int a = focused ? c->focus : c->normal;
            int h = focused ? c->hotfocus : c->hot;
            int def = w->flags & WF_DEFAULT;

            x = scr_puts(y, x, def ? "[< " : "[ ", a);
            x = put_caption(y, x, w->text, a, h);
            scr_puts(y, x, def ? " >]" : " ]", a);
            break;
        }
        }
    }
    scr_shadow(d->y, d->x, d->h, d->w);
}

void dlg_show(struct dialog *d)
{
    place(d);
    if (depth < STACK_MAX)
        stack[depth++] = d;
}

void dlg_hide(struct dialog *d)
{
    int i;

    for (i = 0; i < depth; i++)
        if (stack[i] == d) {
            memmove(stack + i, stack + i + 1,
                    sizeof *stack * (size_t)(depth - i - 1));
            depth--;
            return;
        }
}

void dlg_paint(void)
{
    int i;

    painter();
    scr_cursor(-1, 0);
    for (i = 0; i < depth; i++) {
        place(stack[i]);
        dlg_draw(stack[i]);
    }
}

void dlg_redraw(void)
{
    dlg_paint();
    scr_flush();
}

/* ---- running ---------------------------------------------------------- */

static int focusable(const struct widget *w)
{
    return w->type == W_INPUT || w->type == W_BUTTON || w->type == W_LIST;
}

/* Keys for a focused list. Returns 1 if the key was used; sets *result
 * when a row was chosen. */
static int list_key(struct widget *w, int k, int *result)
{
    int i;

    switch (k) {
    case K_UP:   w->pos--; break;
    case K_DOWN: w->pos++; break;
    case K_PGUP: w->pos -= w->h - 1; break;
    case K_PGDN: w->pos += w->h - 1; break;
    case K_HOME: w->pos = 0; break;
    case K_END:  w->pos = w->cap - 1; break;
    case K_ENTER:
        *result = w->id;
        return 1;
    default:
        if (w->hotkeys == NULL || k <= ' ' || k > 255)
            return 0;
        for (i = 0; i < w->cap; i++)
            if (w->hotkeys[i] == k) {
                w->pos = i;
                *result = w->id;
                return 1;
            }
        return 0;
    }
    if (w->pos >= w->cap)
        w->pos = w->cap - 1;
    if (w->pos < 0)
        w->pos = 0;
    return 1;
}

/* Move focus to the next (dir 1) or previous (-1) focusable widget;
 * with buttons_only, stay among the buttons. */
static void move_focus(struct dialog *d, int dir, int buttons_only)
{
    int i, f = d->focus;

    for (i = 0; i < d->n; i++) {
        f = (f + dir + d->n) % d->n;
        if (focusable(&d->wd[f]) &&
            (!buttons_only || d->wd[f].type == W_BUTTON)) {
            d->focus = f;
            return;
        }
    }
}

static int default_button(const struct dialog *d)
{
    int i, first = -1;

    for (i = 0; i < d->n; i++)
        if (d->wd[i].type == W_BUTTON) {
            if (d->wd[i].flags & WF_DEFAULT)
                return d->wd[i].id;
            if (first < 0)
                first = d->wd[i].id;
        }
    return first;
}

/* A button whose hotkey is `k` (case-insensitive), or -1. */
static int button_for_hotkey(const struct dialog *d, int k)
{
    int i;

    if (k <= 0 || k > 255)
        return -1;
    for (i = 0; i < d->n; i++) {
        const struct widget *w = &d->wd[i];
        int h;

        if (w->type != W_BUTTON)
            continue;
        h = hotkey_of(w->text);
        if (h && (h | 0x20) == (k | 0x20))
            return w->id;
    }
    return -1;
}

/* Line editing for the focused input field. */
static void edit_input(struct widget *w, int k)
{
    int len = (int)strlen(w->buf);

    if (k >= 32 && k < 256 && k != K_BS) {
        if (!w->changed) {          /* first key replaces the old text */
            w->buf[0] = '\0';
            len = w->pos = w->scroll = 0;
        }
        if (len + 1 < w->cap) {
            memmove(w->buf + w->pos + 1, w->buf + w->pos,
                    (size_t)(len - w->pos + 1));
            w->buf[w->pos++] = (char)k;
        }
        w->changed = 1;
        return;
    }
    w->changed = 1;
    switch (k) {
    case K_BS:
        if (w->pos > 0) {
            int start = w->pos - 1;

            while (start > 0 && (w->buf[start] & 0xC0) == 0x80)
                start--;
            memmove(w->buf + start, w->buf + w->pos,
                    (size_t)(len - w->pos + 1));
            w->pos = start;
        }
        break;
    case K_DEL:
        if (w->pos < len) {
            int end = w->pos + 1;

            while ((w->buf[end] & 0xC0) == 0x80)
                end++;
            memmove(w->buf + w->pos, w->buf + end, (size_t)(len - end + 1));
        }
        break;
    case K_LEFT:
        while (w->pos > 0 && (w->buf[--w->pos] & 0xC0) == 0x80)
            ;
        break;
    case K_RIGHT:
        if (w->pos < len)
            while (++w->pos < len && (w->buf[w->pos] & 0xC0) == 0x80)
                ;
        break;
    case K_HOME:
    case K_CTRL('a'):
        w->pos = 0;
        break;
    case K_END:
    case K_CTRL('e'):
        w->pos = len;
        break;
    case K_CTRL('k'):
        w->buf[w->pos] = '\0';
        break;
    case K_CTRL('u'):
        w->buf[0] = '\0';
        w->pos = 0;
        break;
    }
}

/* Mouse click: returns a button id, or -1 (possibly moving focus). */
static int dlg_click(struct dialog *d)
{
    int i, y = key_mouse.y - d->y, x = key_mouse.x - d->x;

    if (key_mouse.release || key_mouse.button != 0)
        return -1;
    for (i = 0; i < d->n; i++) {
        struct widget *w = &d->wd[i];

        if (w->type == W_LIST && y >= w->y && y < w->y + w->h &&
            x >= w->x && x < w->x + w->w) {
            if (w->scroll + y - w->y < w->cap) {
                w->pos = w->scroll + y - w->y;
                return w->id;
            }
            continue;
        }
        if (!focusable(w) || y != w->y || x < w->x || x >= w->x + w->w)
            continue;
        d->focus = i;
        if (w->type == W_BUTTON)
            return w->id;
        w->changed = 1;
        w->pos = (int)strlen(w->buf);
    }
    return -1;
}

int dlg_run(struct dialog *d)
{
    int result = -2;

    if (d->focus < 0 || d->focus >= d->n || !focusable(&d->wd[d->focus])) {
        int i;

        d->focus = -1;
        for (i = 0; i < d->n && d->focus < 0; i++)  /* first input... */
            if (d->wd[i].type == W_INPUT || d->wd[i].type == W_LIST)
                d->focus = i;
        for (i = 0; i < d->n && d->focus < 0; i++)  /* ...or default */
            if (d->wd[i].type == W_BUTTON && (d->wd[i].flags & WF_DEFAULT))
                d->focus = i;
        if (d->focus < 0) {
            d->focus = d->n - 1;
            move_focus(d, 1, 0);
        }
    }
    dlg_show(d);

    while (result == -2) {
        struct widget *w = d->focus >= 0 ? &d->wd[d->focus] : NULL;
        int k, b;

        dlg_redraw();
        k = key_get(-1);
        if (w != NULL && w->type == W_LIST && list_key(w, k, &result))
            continue;
        switch (k) {
        case K_NONE:
            break;
        case K_RESIZE:
            scr_update_size();
            break;
        case K_QUIT:
        case K_ESC:
        case K_CTRL('c'):
        case K_CTRL('g'):
        case K_F(10):
            result = -1;
            break;
        case K_MOUSE:
            b = dlg_click(d);
            if (b >= 0)
                result = b;
            break;
        case K_TAB:
        case K_DOWN:
            move_focus(d, 1, 0);
            break;
        case K_BTAB:
        case K_UP:
            move_focus(d, -1, 0);
            break;
        case K_ENTER:
            result = w != NULL && w->type == W_BUTTON ? w->id
                                                      : default_button(d);
            if (result < 0)
                result = -2;
            break;
        default:
            if (k & K_ALT) {
                b = button_for_hotkey(d, k & ~K_ALT);
                if (b >= 0)
                    result = b;
            } else if (w != NULL && w->type == W_INPUT) {
                edit_input(w, k);
            } else if (k == K_LEFT || k == K_RIGHT) {
                move_focus(d, k == K_LEFT ? -1 : 1, 1);
            } else {
                b = button_for_hotkey(d, k);
                if (b >= 0)
                    result = b;
            }
            break;
        }
    }
    dlg_hide(d);
    return result;
}

/* ---- ready-made dialogs ----------------------------------------------- */

int dlg_query(const char *title, int error, int def, const char *msg,
              const char *const *buttons, int n)
{
    struct dialog d;
    char copy[1024], *lines[16], *s;
    int nl = 0, i, inner = 0, bw = 2 * (n - 1);

    snprintf(copy, sizeof copy, "%s", msg);
    for (s = copy; nl < 16;) {
        lines[nl++] = s;
        s = strchr(s, '\n');
        if (s == NULL)
            break;
        *s++ = '\0';
    }
    for (i = 0; i < nl; i++)
        if (utf8_len(lines[i]) > inner)
            inner = utf8_len(lines[i]);
    for (i = 0; i < n; i++)
        bw += button_width(buttons[i], 0);
    if (bw > inner)
        inner = bw;
    if (title != NULL && utf8_len(title) + 2 > inner)
        inner = utf8_len(title) + 2;
    inner += 4;
    if (inner + 4 > scr_cols)
        inner = scr_cols - 4;

    dlg_init(&d, title, nl + 6, inner + 4, error);
    for (i = 0; i < nl; i++)
        dlg_label(&d, 2 + i, lines[i], 1);
    dlg_hline(&d, 2 + nl);
    /* mc's question boxes draw every button plainly ("[ Yes ]"); the
     * default one is simply the one that starts with the focus. */
    dlg_buttons(&d, 3 + nl, 2, buttons, n, -1);
    for (i = 0; i < d.n; i++)
        if (d.wd[i].type == W_BUTTON && d.wd[i].id == def)
            d.focus = i;
    return dlg_run(&d);
}

int dlg_list(const char *title, const char *const *items,
             const char *hotkeys, int n)
{
    struct dialog d;
    struct widget *w;
    int i, width = 0, h = n;

    for (i = 0; i < n; i++)
        if (utf8_len(items[i]) > width)
            width = utf8_len(items[i]);
    width += 2;
    if (title != NULL && utf8_len(title) + 6 > width)
        width = utf8_len(title) + 6;
    width += 6;
    if (width > scr_cols - 2)
        width = scr_cols - 2;
    if (h > scr_rows - 6)
        h = scr_rows - 6;
    if (h < 1)
        return -1;

    dlg_init(&d, title, h + 4, width, DLG_MENU);
    w = dlg_list_box(&d, 2, h, items, hotkeys, n);
    return dlg_run(&d) == 0 ? w->pos : -1;
}

void dlg_message(const char *title, int error, const char *fmt, ...)
{
    static const char *const ok[] = { "&OK" };
    char msg[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    dlg_query(title, error, 0, msg, ok, 1);
}

int dlg_input_box(const char *title, const char *prompt, char *buf, int cap,
                  int width)
{
    static const char *const btn[] = { "&OK", "&Cancel" };
    struct dialog d;
    int y = 2;

    if (width <= 0)
        width = 40;
    if (prompt != NULL && utf8_len(prompt) + 8 > width)
        width = utf8_len(prompt) + 8;
    if (width > scr_cols - 2)
        width = scr_cols - 2;
    dlg_init(&d, title, prompt != NULL ? 8 : 7, width, 0);
    if (prompt != NULL)
        dlg_label(&d, y++, prompt, 0);
    dlg_input(&d, y++, buf, cap);
    dlg_hline(&d, y++);
    dlg_buttons(&d, y, 1, btn, 2, 0);
    return dlg_run(&d) == 0;
}
