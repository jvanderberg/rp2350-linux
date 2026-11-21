/*
 * menu.c - menu bar and pull-down menus. See menu.h.
 *
 * Geometry follows mc: titles start at column 2 with three spaces
 * between " Title " cells; a pull-down opens one column left of its
 * title with labels in one column and shortcuts aligned in another.
 */
#include <stdio.h>
#include <string.h>

#include "dialog.h"
#include "keys.h"
#include "menu.h"
#include "screen.h"
#include "skin.h"
#include "util.h"

static int text_len(const char *s)
{
    return utf8_len(s) - (strchr(s, '&') != NULL);
}

/* Column where the " Title " cell of menu i starts. */
static int title_x(const struct menu *m, int i)
{
    int x = 1, j;

    for (j = 0; j < i; j++)
        x += text_len(m[j].title) + 2 + 3;
    return x;
}

/* Draw `s`, showing the letter after '&' in `hot`. */
static int put_hot(int y, int x, const char *s, int attr, int hot)
{
    int next_hot = 0;

    while (*s) {
        unsigned c = utf8_next(&s);

        if (c == '&') {
            next_hot = 1;
            continue;
        }
        scr_put(y, x++, c, next_hot ? hot : attr);
        next_hot = 0;
    }
    return x;
}

void menubar_draw(const struct menu *menus, int n, int open)
{
    int i;

    if (open < 0) {
        scr_fill(0, 0, 1, scr_cols, ' ', A_MENUBAR);
        for (i = 0; i < n; i++)
            put_hot(0, title_x(menus, i) + 1, menus[i].title, A_MENUBAR,
                    A_MENUBAR);
        return;
    }
    scr_fill(0, 0, 1, scr_cols, ' ', A_MENU);
    for (i = 0; i < n; i++) {
        int sel = i == open, x = title_x(menus, i);
        int a = sel ? A_MENU_SEL : A_MENU, h = sel ? A_MENU_HOTSEL : A_MENU_HOT;

        scr_put(0, x, ' ', a);
        x = put_hot(0, x + 1, menus[i].title, a, h);
        scr_put(0, x, ' ', a);
    }
}

void keybar_draw(int y, const char *const labels[10])
{
    int i, slot = scr_cols / 10, x = 0;
    char num[4];

    for (i = 0; i < 10; i++) {
        int w = i == 9 ? scr_cols - x : slot;

        sprintf(num, "%2d", i + 1);
        scr_text(y, x, 2, num, A_KEY_NUM, T_LEFT);
        scr_text(y, x + 2, w - 2, labels[i], A_KEY_LABEL, T_LEFT);
        x += w;
    }
}

int keybar_hit(int x)
{
    int slot = x / (scr_cols / 10 > 0 ? scr_cols / 10 : 1);

    return slot > 9 ? 10 : slot + 1;
}

int menubar_hit(const struct menu *menus, int n, int x)
{
    int i;

    for (i = 0; i < n; i++) {
        int tx = title_x(menus, i);

        if (x >= tx && x < tx + text_len(menus[i].title) + 2)
            return i;
    }
    return -1;
}

/* Pull-down geometry: box position, size and the shortcut column. */
struct pulldown {
    int x, w, h, keys_x;
};

static void pulldown_geometry(const struct menu *menus, int which,
                              struct pulldown *g)
{
    const struct menu *m = &menus[which];
    int i, label_w = 0, keys_w = 0, inner;

    for (i = 0; i < m->n; i++) {
        if (m->items[i].label == NULL)
            continue;
        if (text_len(m->items[i].label) > label_w)
            label_w = text_len(m->items[i].label);
        if (m->items[i].keys && utf8_len(m->items[i].keys) > keys_w)
            keys_w = utf8_len(m->items[i].keys);
    }
    inner = 1 + label_w + (keys_w ? 2 + keys_w : 1);
    g->w = inner + 2;
    g->h = m->n + 2;
    g->x = title_x(menus, which) - 1;
    if (g->x + g->w > scr_cols)
        g->x = scr_cols - g->w;
    if (g->x < 0)
        g->x = 0;
    g->keys_x = g->x + 1 + 1 + label_w + 2;
}

static void pulldown_draw(const struct menu *menus, int which, int sel)
{
    const struct menu *m = &menus[which];
    struct pulldown g;
    int i;

    pulldown_geometry(menus, which, &g);
    scr_fill(1, g.x, g.h, g.w, ' ', A_MENU);
    scr_box(1, g.x, g.h, g.w, A_MENU);
    for (i = 0; i < m->n; i++) {
        const struct menu_item *it = &m->items[i];
        int y = 2 + i, s = i == sel;
        int a = s ? A_MENU_SEL : A_MENU, h = s ? A_MENU_HOTSEL : A_MENU_HOT;

        if (it->label == NULL) {
            scr_put(y, g.x, BOX_LT, A_MENU);
            scr_hline(y, g.x + 1, g.w - 2, BOX_H, A_MENU);
            scr_put(y, g.x + g.w - 1, BOX_RT, A_MENU);
            continue;
        }
        scr_fill(y, g.x + 1, 1, g.w - 2, ' ', a);
        put_hot(y, g.x + 2, it->label, a, h);
        if (it->keys != NULL)
            scr_puts(y, g.keys_x, it->keys, a);
    }
    scr_shadow(1, g.x, g.h, g.w);
}

/* Next selectable item from `i` in direction `dir` (wrapping). */
static int step(const struct menu *m, int i, int dir)
{
    int k;

    for (k = 0; k < m->n; k++) {
        i = (i + dir + m->n) % m->n;
        if (m->items[i].label != NULL)
            return i;
    }
    return 0;
}

static int item_for_hotkey(const struct menu *m, int k)
{
    int i;

    for (i = 0; i < m->n; i++) {
        const char *amp;

        if (m->items[i].label == NULL)
            continue;
        amp = strchr(m->items[i].label, '&');
        if (amp != NULL && (amp[1] | 0x20) == (k | 0x20))
            return i;
    }
    return -1;
}

int menu_run(const struct menu *menus, int n, int *which)
{
    int m = *which, sel = step(&menus[m], -1, 1), k, i;

    for (;;) {
        dlg_paint();                /* main screen underneath */
        menubar_draw(menus, n, m);
        pulldown_draw(menus, m, sel);
        scr_cursor(-1, 0);
        scr_flush();

        k = key_get(-1);
        switch (k) {
        case K_RESIZE:
            scr_update_size();
            break;
        case K_QUIT:
        case K_ESC:
        case K_F(9):
        case K_F(10):
        case K_CTRL('c'):
        case K_CTRL('g'):
            return 0;
        case K_LEFT:
            m = (m + n - 1) % n;
            sel = step(&menus[m], -1, 1);
            break;
        case K_RIGHT:
            m = (m + 1) % n;
            sel = step(&menus[m], -1, 1);
            break;
        case K_UP:
            sel = step(&menus[m], sel, -1);
            break;
        case K_DOWN:
            sel = step(&menus[m], sel, 1);
            break;
        case K_HOME:
        case K_PGUP:
            sel = step(&menus[m], -1, 1);
            break;
        case K_END:
        case K_PGDN:
            sel = step(&menus[m], menus[m].n, -1);
            break;
        case K_ENTER:
            *which = m;
            return menus[m].items[sel].cmd;
        case K_MOUSE: {
            struct pulldown g;

            if (key_mouse.release || key_mouse.button != 0)
                break;
            if (key_mouse.y == 0) {
                i = menubar_hit(menus, n, key_mouse.x);
                if (i < 0)
                    return 0;
                m = i;
                sel = step(&menus[m], -1, 1);
                break;
            }
            pulldown_geometry(menus, m, &g);
            i = key_mouse.y - 2;
            if (key_mouse.x > g.x && key_mouse.x < g.x + g.w - 1 &&
                i >= 0 && i < menus[m].n) {
                if (menus[m].items[i].label == NULL)
                    break;
                *which = m;
                return menus[m].items[i].cmd;
            }
            return 0;               /* click outside closes the menu */
        }
        default:
            if (k > 0 && k < 256) {
                i = item_for_hotkey(&menus[m], k);
                if (i >= 0) {
                    *which = m;
                    return menus[m].items[i].cmd;
                }
            }
            break;
        }
    }
}
