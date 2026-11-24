/*
 * panel_draw.c - drawing a panel on the terminal. See panel.h for the
 * layout. The listing, sorting and marks are in panel.c, which xmc shares.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "panel.h"
#include "screen.h"
#include "skin.h"
#include "util.h"

/* The one-character type marker mc puts in front of each name. */
static char entry_mark_char(const struct entry *e)
{
    if (e->flags & E_UPDIR || S_ISDIR(e->mode))
        return '/';
    if (e->flags & E_LINKDIR)
        return '~';
    if (e->flags & E_STALE)
        return '!';
    if (e->flags & E_LINK)
        return '@';
    if (S_ISFIFO(e->mode))
        return '|';
    if (S_ISSOCK(e->mode))
        return '=';
    if (S_ISCHR(e->mode))
        return '-';
    if (S_ISBLK(e->mode))
        return '+';
    if (e->flags & E_EXEC)
        return '*';
    return ' ';
}

/* ---- drawing ---------------------------------------------------------- */

/* Column layout inside the frame: x and width of name, size and date.
 * Narrow panels drop the date, then the size. */
struct columns {
    int nx, nw, sx, sw, tx, tw;
};

static void columns(const struct panel *p, struct columns *c)
{
    int inner = p->w - 2;

    c->sw = 7;
    c->tw = 12;
    if (inner < 8 + 1 + c->sw + 1 + c->tw)
        c->tw = 0;
    if (inner < 8 + 1 + c->sw)
        c->sw = 0;
    c->nw = inner - (c->sw ? c->sw + 1 : 0) - (c->tw ? c->tw + 1 : 0);
    c->nx = p->x + 1;
    c->sx = c->nx + c->nw + 1;
    c->tx = c->sx + c->sw + 1;
}

static void draw_entry(const struct panel *p, const struct columns *c, int y,
                       int idx, int is_cursor)
{
    const struct entry *e = &p->e[idx];
    int marked = e->flags & E_MARKED;
    int attr, sep = is_cursor ? A_CURSOR : A_PANEL;
    char buf[32];

    if (is_cursor)
        attr = marked ? A_MARKED_CUR : A_CURSOR;
    else
        attr = marked ? A_MARKED : ATTR(entry_color(e), C_BLUE);

    scr_put(y, c->nx, (unsigned char)entry_mark_char(e), attr);
    scr_text(y, c->nx + 1, c->nw - 1, e->name, attr, T_LEFT | T_TRIM_MID);
    if (c->sw) {
        scr_put(y, c->sx - 1, BOX_V, sep);
        if (e->flags & E_UPDIR)
            strcpy(buf, "UP--DIR");
        else
            fmt_size(buf, c->sw, e->size);
        scr_text(y, c->sx, c->sw, buf, attr, T_RIGHT);
    }
    if (c->tw) {
        scr_put(y, c->tx - 1, BOX_V, sep);
        fmt_time(buf, e->mtime);
        scr_text(y, c->tx, c->tw, buf, attr, T_LEFT);
    }
}

/* The line between the list and the mini status. */
static void draw_separator(const struct panel *p, int y)
{
    char num[32], buf[64];

    scr_put(y, p->x, BOX_LT, A_PANEL);
    scr_hline(y, p->x + 1, p->w - 2, BOX_H, A_PANEL);
    scr_put(y, p->x + p->w - 1, BOX_RT, A_PANEL);
    if (p->nmarked > 0) {
        int len;

        fmt_commas(num, (unsigned long long)p->marked_bytes);
        len = snprintf(buf, sizeof buf, " %s B in %d file%s ", num,
                       p->nmarked, p->nmarked == 1 ? "" : "s");
        if (len > p->w - 2)
            len = p->w - 2;
        scr_text(y, p->x + (p->w - len) / 2, len, buf, A_MARKED, T_LEFT);
    }
}

/* The line under the separator: what the cursor is on, or the search. */
static void draw_mini_status(const struct panel *p, int y)
{
    int x = p->x + 1, w = p->w - 2;
    char buf[PATH_MAX + 8];

    if (p->searching) {
        snprintf(buf, sizeof buf, "/%s", p->search);
        scr_text(y, x, w, buf, A_CURSOR, T_LEFT);
        return;
    }
    if (p->n == 0)
        return;
    {
        const struct entry *e = &p->e[p->cur];

        if (e->flags & E_UPDIR) {
            strcpy(buf, "UP--DIR");
        } else if (e->flags & E_LINK) {
            char tgt[PATH_MAX], full[PATH_MAX];
            ssize_t n = -1;

            if (path_join(full, sizeof full, p->path, e->name) == 0)
                n = readlink(full, tgt, sizeof tgt - 1);
            tgt[n > 0 ? n : 0] = '\0';
            snprintf(buf, sizeof buf, "-> %s", tgt);
        } else {
            snprintf(buf, sizeof buf, "%c%s", entry_mark_char(e), e->name);
        }
        scr_text(y, x, w, buf, A_PANEL, T_LEFT | T_TRIM_MID);
    }
}

/* Free space, right-aligned in the bottom frame line. */
static void draw_free_space(const struct panel *p, int y)
{
    char fr[16], tot[16], buf[64];
    int len;

    if (p->fs_total <= 0)
        return;
    fmt_short(fr, p->fs_free);
    fmt_short(tot, p->fs_total);
    len = snprintf(buf, sizeof buf, " %s / %s (%d%%) ", fr, tot,
                   (int)(p->fs_free * 100 / p->fs_total));
    if (len <= p->w - 4)
        scr_puts(y, p->x + p->w - 2 - len, buf, A_PANEL);
}

void panel_draw(const struct panel *p, int active)
{
    static const char sort_chars[] = "nesm";
    struct columns c;
    int x = p->x, y = p->y, w = p->w, h = p->h;
    int rows = panel_list_rows(p), r, tw;
    char hdr[3];

    if (w < 8 || h < 6)
        return;
    columns(p, &c);
    scr_fill(y, x, h, w, ' ', A_PANEL);
    scr_box(y, x, h, w, A_PANEL);

    /* Title: "┌<─ /the/path ─────.[^]>┐"; the path is highlighted in
     * the active panel and shortened from the left when too long. */
    scr_put(y, x + 1, '<', A_PANEL);
    scr_puts(y, x + w - 6, ".[^]>", A_PANEL);
    tw = utf8_len(p->path);
    if (tw > w - 12)
        tw = w - 12;
    if (tw > 0) {
        int ta = active ? A_TITLE_ACTIVE : A_PANEL;

        scr_put(y, x + 3, ' ', ta);
        scr_text(y, x + 4, tw, p->path, ta, T_LEFT | T_TRIM_START);
        scr_put(y, x + 4 + tw, ' ', ta);
    }

    /* Column headers, with the sort order indicator (".n" = by name). */
    hdr[0] = p->reverse ? '\'' : '.';
    hdr[1] = sort_chars[p->sort];
    hdr[2] = '\0';
    scr_text(y + 1, c.nx, c.nw, "", A_HEADER, T_LEFT);
    scr_puts(y + 1, c.nx + (c.nw - 4 + 1) / 2, "Name", A_HEADER);
    scr_puts(y + 1, c.nx, hdr, A_HEADER);
    if (c.sw)
        scr_text(y + 1, c.sx, c.sw, "Size", A_HEADER, T_CENTER);
    if (c.tw)
        scr_text(y + 1, c.tx, c.tw, "Modify time", A_HEADER, T_CENTER);

    /* Column dividers run from the header to the separator. */
    for (r = y + 1; r < y + 2 + rows; r++) {
        if (c.sw)
            scr_put(r, c.sx - 1, BOX_V, A_PANEL);
        if (c.tw)
            scr_put(r, c.tx - 1, BOX_V, A_PANEL);
    }

    for (r = 0; r < rows && p->top + r < p->n; r++)
        draw_entry(p, &c, y + 2 + r, p->top + r,
                   active && p->top + r == p->cur);

    draw_separator(p, y + h - 3);
    draw_mini_status(p, y + h - 2);
    draw_free_space(p, y + h - 1);
}
