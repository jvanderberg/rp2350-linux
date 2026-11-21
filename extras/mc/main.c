/*
 * main.c - a Midnight Commander lookalike: two panels, F-key bar, menus.
 *
 * Screen layout (rows):
 *
 *   0          menu bar         Left  File  Command  Options  Right
 *   1..        two panels       see panel.h
 *   rows-3     hint line        "Hint: ..."
 *   rows-2     command line     "$ " + whatever is typed
 *   rows-1     key bar          1Help 2Menu 3View ... 10Quit
 *
 * Each key press is turned into a command (enum cmd), and every
 * iteration redraws the whole screen into the back buffer; screen.c
 * sends only what changed. Modules:
 *
 *   screen.c   cell buffer + terminal       keys.c     input decoding
 *   panel.c    directory panels             dialog.c   dialogs
 *   menu.c     menu bar / pull-downs        fileops.c  copy/move/delete
 *   run.c      running other programs       util.c     helpers
 *   viewer.c   F3 viewer (+ Markdown)       usermenu.c F2 user menu
 *   skin.h     all colours
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "dialog.h"
#include "fileops.h"
#include "keys.h"
#include "menu.h"
#include "panel.h"
#include "run.h"
#include "screen.h"
#include "skin.h"
#include "usermenu.h"
#include "util.h"
#include "viewer.h"

/* Everything the user can ask for, from keys, menus or the mouse. */
enum cmd {
    CMD_NONE,
    CMD_HELP, CMD_USERMENU, CMD_VIEW, CMD_EDIT, CMD_COPY, CMD_MOVE,
    CMD_MKDIR, CMD_DELETE, CMD_MENU, CMD_QUIT,
    CMD_SELECT, CMD_UNSELECT, CMD_INVERT,
    CMD_SWAP, CMD_RESCAN, CMD_COMPARE, CMD_OUTPUT, CMD_SEARCH,
    CMD_SORT_NAME, CMD_SORT_EXT, CMD_SORT_SIZE, CMD_SORT_MTIME,
    CMD_SORT_REVERSE,
    CMD_HIDDEN, CMD_MIX, CMD_HINTBAR, CMD_INTERNAL_VIEW,
    CMD_OTHER_TO_CURSOR, CMD_OTHER_TO_HERE
};

/* ---- menus ------------------------------------------------------------ */

static const struct menu_item panel_items[] = {
    { "Sort by &name", NULL, CMD_SORT_NAME },
    { "Sort by &extension", NULL, CMD_SORT_EXT },
    { "Sort by si&ze", NULL, CMD_SORT_SIZE },
    { "Sort by &modify time", NULL, CMD_SORT_MTIME },
    { "Re&verse order", NULL, CMD_SORT_REVERSE },
    { NULL, NULL, 0 },
    { "&Rescan", "C-r", CMD_RESCAN },
};

static const struct menu_item file_items[] = {
    { "&View", "F3", CMD_VIEW },
    { "&Edit", "F4", CMD_EDIT },
    { "&Copy", "F5", CMD_COPY },
    { "&Rename/Move", "F6", CMD_MOVE },
    { "&Mkdir", "F7", CMD_MKDIR },
    { "&Delete", "F8", CMD_DELETE },
    { NULL, NULL, 0 },
    { "&Select group", "+", CMD_SELECT },
    { "U&nselect group", "-", CMD_UNSELECT },
    { "&Invert selection", "*", CMD_INVERT },
    { NULL, NULL, 0 },
    { "E&xit", "F10", CMD_QUIT },
};

static const struct menu_item command_items[] = {
    { "&Swap panels", "C-u", CMD_SWAP },
    { "S&witch panels on/off", "C-o", CMD_OUTPUT },
    { "&Compare directories", "C-x d", CMD_COMPARE },
    { "&Quick search", "C-s", CMD_SEARCH },
    { NULL, NULL, 0 },
    { "Other panel to &cursor", "M-o", CMD_OTHER_TO_CURSOR },
    { "Other panel to &here", "M-i", CMD_OTHER_TO_HERE },
};

static const struct menu_item options_items[] = {
    { "Show &hidden files", "M-.", CMD_HIDDEN },
    { "&Mix all files", NULL, CMD_MIX },
    { "Hint &bar visible", NULL, CMD_HINTBAR },
    { "Use internal &viewer", NULL, CMD_INTERNAL_VIEW },
};

#define COUNT(a) ((int)(sizeof (a) / sizeof (a)[0]))

static const struct menu menus[] = {
    { "&Left", panel_items, COUNT(panel_items) },
    { "&File", file_items, COUNT(file_items) },
    { "&Command", command_items, COUNT(command_items) },
    { "&Options", options_items, COUNT(options_items) },
    { "&Right", panel_items, COUNT(panel_items) },
};
#define MENU_LEFT  0
#define MENU_RIGHT 4

/* ---- state ------------------------------------------------------------ */

static struct panel panels[2];
static int active;                  /* index of the active panel */
static char cmdline[1024];          /* the command line being typed */
static int quit;
static int ctrl_x;                  /* C-x prefix pending */
static int hint_bar = 1;            /* Options > Hint bar visible */
static int internal_view = 1;       /* F3: built-in viewer, or $PAGER */
static int hint_no;
static int last_click_idx = -1;     /* for double-click detection */
static long long last_click_us;

static const char *const hints[] = {
    "Hint: Mark files with Insert or C-t; F5, F6 and F8 then act on all of them.",
    "Hint: Press C-o to see the output of the last command.",
    "Hint: M-o shows the directory under the cursor in the other panel.",
    "Hint: C-s starts a quick search; keep typing to narrow it down.",
    "Hint: Esc followed by a digit works like the F key: Esc 5 is F5.",
    "Hint: Type a command and press Enter to run it here; cd works too.",
    "Hint: C-u swaps the panels; C-x d marks the differences between them.",
    "Hint: Use + and - to mark or unmark files matching a pattern.",
};

static struct panel *cur_panel(void)
{
    return &panels[active];
}

static struct panel *other_panel(void)
{
    return &panels[!active];
}

/* ---- drawing ---------------------------------------------------------- */

static int show_hint(void)
{
    return hint_bar && scr_rows >= 16;
}

static void layout(void)
{
    int h = scr_rows - 3 - show_hint();

    panels[0].x = 0;
    panels[0].w = scr_cols / 2;
    panels[1].x = scr_cols / 2;
    panels[1].w = scr_cols - scr_cols / 2;
    panels[0].y = panels[1].y = 1;
    panels[0].h = panels[1].h = h;
    /* keep the cursors in view after a resize */
    panel_goto(&panels[0], panels[0].cur);
    panel_goto(&panels[1], panels[1].cur);
}

static void draw_command_line(int y)
{
    const char *prompt = geteuid() == 0 ? "# " : "$ ";
    int len = utf8_len(cmdline), room = scr_cols - 3;
    const char *s = cmdline;

    /* when the line is too long, show its end */
    while (len > room) {
        utf8_next(&s);
        len--;
    }
    scr_puts(y, 0, prompt, A_SHELL);
    scr_puts(y, 2, s, A_SHELL);
    scr_cursor(y, 2 + len);
}

void app_draw(void)
{
    layout();
    scr_fill(0, 0, scr_rows, scr_cols, ' ', A_SHELL);
    menubar_draw(menus, COUNT(menus), -1);
    panel_draw(&panels[0], active == 0);
    panel_draw(&panels[1], active == 1);
    if (show_hint())
        scr_text(scr_rows - 3, 0, scr_cols, hints[hint_no], A_SHELL, T_LEFT);
    static const char *const keys[10] = {
        "Help", "Menu", "View", "Edit", "Copy",
        "RenMov", "Mkdir", "Delete", "PullDn", "Quit"
    };

    draw_command_line(scr_rows - 2);
    keybar_draw(scr_rows - 1, keys);
}

/* ---- commands ---------------------------------------------------------- */

static void error_box(const char *what, const char *path)
{
    dlg_message("Error", 1, "%s\n\"%s\"\n%s", what, path, strerror(errno));
}

static void chdir_panel(struct panel *p, const char *dir)
{
    if (panel_chdir(p, dir) != 0)
        error_box("Cannot change directory", dir);
    else
        hint_no = (hint_no + 1) % COUNT(hints);
}

static void reload_both(void)
{
    panel_load(&panels[0]);
    panel_load(&panels[1]);
}

static void show_help(void)
{
    static const char *const lines[] = {
        "F2         user menu          F3 on *.md  formatted Markdown",
        "Tab        other panel        Insert C-t  mark file",
        "Enter      open / run         + - *       mark group",
        "F3  View   F4  Edit           C-s         quick search",
        "F5  Copy   F6  Rename/Move    C-r         rescan",
        "F7  Mkdir  F8  Delete         C-u         swap panels",
        "F9  Menu   F10 Quit           C-o         command output",
        "M-.        hidden files       M-o M-i     other panel",
        "C-x d      compare dirs       C-PgUp      parent dir",
        "Esc 1..0   same as F1..F10    M-Enter     name to cmdline",
        "",
        "Type a command and press Enter to run it (cd works too).",
        "Mouse: click selects, double-click opens, right-click marks.",
    };
    static const char *const ok[] = { "&OK" };
    struct dialog d;
    int i, n = COUNT(lines);

    dlg_init(&d, "Help", n + 6, 66, 0);
    for (i = 0; i < n; i++)
        dlg_label(&d, 2 + i, lines[i], 0);
    dlg_hline(&d, 2 + n);
    dlg_buttons(&d, 3 + n, 1, ok, 1, 0);
    dlg_run(&d);
}

/* Does the file start like a program (ELF or #!)? FAT marks every file
 * executable, so Enter checks before trying to run something. */
static int looks_runnable(const char *dir, const char *name)
{
    char path[PATH_MAX];
    unsigned char b[4] = { 0 };
    int fd;

    if (path_join(path, sizeof path, dir, name) != 0)
        return 0;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    if (read(fd, b, sizeof b) < 2)
        b[0] = 0;
    close(fd);
    return (b[0] == 0x7F && b[1] == 'E' && b[2] == 'L' && b[3] == 'F') ||
           (b[0] == '#' && b[1] == '!');
}

static void view_current(struct panel *p)
{
    struct entry *e = panel_current(p);
    char path[PATH_MAX];

    if (e == NULL)
        return;
    if (entry_is_dir(e))
        chdir_panel(p, e->name);
    else if (!internal_view)
        run_with(p->path, "PAGER", "less", e->name);
    else if (path_join(path, sizeof path, p->path, e->name) == 0 &&
             view_file(path) != 0)
        error_box("Cannot view", path);
}

/* Enter on an entry: open a directory, run a program, view the rest. */
static void open_current(struct panel *p)
{
    struct entry *e = panel_current(p);
    char cmd[PATH_MAX + 4];

    if (e == NULL)
        return;
    if (entry_is_dir(e)) {
        chdir_panel(p, e->name);
    } else if ((e->flags & E_EXEC) && looks_runnable(p->path, e->name)) {
        snprintf(cmd, sizeof cmd, "./%s", e->name);
        run_command(p->path, cmd);
        panel_load(p);
    } else {
        view_current(p);
    }
}

/* Run what was typed on the command line; "cd" is handled here. */
static void execute_command_line(void)
{
    struct panel *p = cur_panel();
    char *s = cmdline;

    while (*s == ' ')
        s++;
    if (strncmp(s, "cd", 2) == 0 && (s[2] == '\0' || s[2] == ' ')) {
        const char *dir = s + 2;
        char buf[PATH_MAX];

        while (*dir == ' ')
            dir++;
        if (*dir == '\0' || strcmp(dir, "~") == 0)
            dir = getenv("HOME") ? getenv("HOME") : "/";
        else if (strncmp(dir, "~/", 2) == 0 && getenv("HOME")) {
            snprintf(buf, sizeof buf, "%s/%s", getenv("HOME"), dir + 2);
            dir = buf;
        }
        chdir_panel(p, dir);
    } else if (*s != '\0') {
        run_command(p->path, s);
        reload_both();
    }
    cmdline[0] = '\0';
}

static void select_group(struct panel *p, int on)
{
    static char pattern[256] = "*";

    if (dlg_input_box(on ? "Select" : "Unselect", NULL, pattern,
                      sizeof pattern, 40))
        panel_mark_glob(p, pattern, on);
}

/* C-x d: mark files that exist only on one side or differ (by size, or
 * the newer one by date) - mc's "quick" comparison. */
static void compare_dirs(void)
{
    struct panel *a = &panels[0], *b = &panels[1];
    int i, j;

    panel_unmark_all(a);
    panel_unmark_all(b);
    for (i = 0; i < a->n; i++) {
        struct entry *ea = &a->e[i];

        if (entry_is_dir(ea))
            continue;
        j = panel_find(b, ea->name);
        if (j < 0 || entry_is_dir(&b->e[j])) {
            panel_mark(a, i, 1);
            continue;
        }
        if (ea->size != b->e[j].size && ea->mtime == b->e[j].mtime) {
            panel_mark(a, i, 1);
            panel_mark(b, j, 1);
        } else if (ea->mtime > b->e[j].mtime) {
            panel_mark(a, i, 1);
        } else if (ea->mtime < b->e[j].mtime) {
            panel_mark(b, j, 1);
        }
    }
    for (j = 0; j < b->n; j++)
        if (!entry_is_dir(&b->e[j]) && panel_find(a, b->e[j].name) < 0)
            panel_mark(b, j, 1);
}

static void swap_panels(void)
{
    struct panel tmp = panels[0];

    panels[0] = panels[1];
    panels[1] = tmp;
    active = !active;
}

static void set_sort(struct panel *p, int key)
{
    p->sort = key;
    panel_load(p);
}

static void confirm_quit(void)
{
    static const char *const btn[] = { "&Yes", "&No" };

    if (dlg_query("The Midnight Commander", 0, 0,
                  "Do you really want to quit the Midnight Commander?",
                  btn, 2) == 0)
        quit = 1;
}

/* Perform a command. `p` is the panel it applies to (the active one,
 * or the one whose Left/Right menu it came from). */
static void do_command(int cmd, struct panel *p)
{
    struct entry *e = panel_current(p);
    int which;

    switch (cmd) {
    case CMD_HELP:      show_help(); break;
    case CMD_USERMENU:
        if (usermenu_run(p, p == &panels[0] ? &panels[1] : &panels[0]))
            reload_both();
        break;
    case CMD_VIEW:      view_current(p); break;
    case CMD_EDIT:
        if (e != NULL && !entry_is_dir(e)) {
            run_with(p->path, "EDITOR", "vi", e->name);
            panel_load(p);
        }
        break;
    case CMD_COPY:
    case CMD_MOVE:
        if (fop_copy(p, p == cur_panel() ? other_panel() : cur_panel(),
                     cmd == CMD_MOVE))
            reload_both();
        break;
    case CMD_MKDIR:
        if (fop_mkdir(p))
            panel_load(p == cur_panel() ? other_panel() : cur_panel());
        break;
    case CMD_DELETE:
        if (fop_delete(p))
            reload_both();
        break;
    case CMD_MENU:
        which = active == 0 ? MENU_LEFT : MENU_RIGHT;
        cmd = menu_run(menus, COUNT(menus), &which);
        if (cmd != CMD_NONE)
            do_command(cmd, which == MENU_LEFT ? &panels[0]
                          : which == MENU_RIGHT ? &panels[1] : cur_panel());
        break;
    case CMD_QUIT:      confirm_quit(); break;
    case CMD_SELECT:    select_group(p, 1); break;
    case CMD_UNSELECT:  select_group(p, 0); break;
    case CMD_INVERT:    panel_invert(p); break;
    case CMD_SWAP:      swap_panels(); break;
    case CMD_RESCAN:    panel_load(p); break;
    case CMD_COMPARE:   compare_dirs(); break;
    case CMD_OUTPUT:    run_show_output(); break;
    case CMD_SEARCH:
        p->searching = 1;
        p->search[0] = '\0';
        break;
    case CMD_SORT_NAME:  set_sort(p, SORT_NAME); break;
    case CMD_SORT_EXT:   set_sort(p, SORT_EXT); break;
    case CMD_SORT_SIZE:  set_sort(p, SORT_SIZE); break;
    case CMD_SORT_MTIME: set_sort(p, SORT_MTIME); break;
    case CMD_SORT_REVERSE:
        p->reverse = !p->reverse;
        panel_load(p);
        break;
    case CMD_HIDDEN:
        opt_show_hidden = !opt_show_hidden;
        reload_both();
        break;
    case CMD_MIX:
        opt_mix_files = !opt_mix_files;
        reload_both();
        break;
    case CMD_HINTBAR:
        hint_bar = !hint_bar;
        break;
    case CMD_INTERNAL_VIEW:
        internal_view = !internal_view;
        break;
    case CMD_OTHER_TO_CURSOR: {
        char path[PATH_MAX];

        if (e != NULL && entry_is_dir(e) &&
            path_join(path, sizeof path, p->path, e->name) == 0)
            chdir_panel(other_panel(), path);
        else
            chdir_panel(other_panel(), p->path);
        break;
    }
    case CMD_OTHER_TO_HERE:
        chdir_panel(other_panel(), p->path);
        break;
    }
}

/* ---- input ------------------------------------------------------------ */

/* Keys while quick search is on. Returns 1 if the key was used up. */
static int search_key(struct panel *p, int k)
{
    size_t len = strlen(p->search);

    if (k == K_CTRL('s') || k == (K_ALT | 's')) {
        if (len > 0)
            panel_search(p, p->cur + 1);
        return 1;
    }
    if (k == K_BS) {
        if (len > 0)
            p->search[len - 1] = '\0';
        return 1;
    }
    if (k >= 32 && k < 256 && k != K_BS) {
        if (len + 1 < sizeof p->search) {
            p->search[len] = (char)k;
            p->search[len + 1] = '\0';
            if (!panel_search(p, p->cur))
                p->search[len] = '\0';  /* no match: refuse the char */
        }
        return 1;
    }
    p->searching = 0;
    return k == K_ESC;      /* other keys end the search and still act */
}

static void handle_mouse(void)
{
    struct mouse_event *m = &key_mouse;
    int i, idx;
    struct panel *p;

    if (m->release)
        return;
    if (m->y == 0 && m->button == 0) {          /* menu bar */
        int which = menubar_hit(menus, COUNT(menus), m->x);

        if (which >= 0) {
            int cmd = menu_run(menus, COUNT(menus), &which);

            if (cmd != CMD_NONE)
                do_command(cmd, which == MENU_LEFT ? &panels[0]
                              : which == MENU_RIGHT ? &panels[1]
                              : cur_panel());
        }
        return;
    }
    if (m->y == scr_rows - 1 && m->button == 0) {   /* key bar */
        static const int fkeys[10] = {
            CMD_HELP, CMD_USERMENU, CMD_VIEW, CMD_EDIT, CMD_COPY,
            CMD_MOVE, CMD_MKDIR, CMD_DELETE, CMD_MENU, CMD_QUIT
        };

        do_command(fkeys[keybar_hit(m->x) - 1], cur_panel());
        return;
    }
    for (i = 0; i < 2; i++) {
        p = &panels[i];
        if (m->x < p->x || m->x >= p->x + p->w || m->y < p->y ||
            m->y >= p->y + p->h)
            continue;
        active = i;
        p->searching = 0;
        if (m->button == 64 || m->button == 65) {   /* wheel */
            panel_move(p, m->button == 64 ? -2 : 2);
            return;
        }
        idx = panel_hit(p, m->y, m->x);
        if (idx < 0)
            return;
        panel_goto(p, idx);
        if (m->button == 2) {                       /* right: mark */
            panel_mark(p, idx, !(p->e[idx].flags & E_MARKED));
        } else if (m->button == 0) {
            long long t = now_us();

            if (idx == last_click_idx && t - last_click_us < 400000) {
                last_click_idx = -1;
                open_current(p);
            } else {
                last_click_idx = idx;
                last_click_us = t;
            }
        }
        return;
    }
}

static void handle_key(int k)
{
    struct panel *p = cur_panel();
    int rows = panel_list_rows(p);
    size_t len = strlen(cmdline);

    if (p->searching && search_key(p, k))
        return;
    if (ctrl_x) {               /* second key of a C-x sequence */
        ctrl_x = 0;
        if (k == 'd' || k == K_CTRL('d'))
            compare_dirs();
        return;
    }
    if (k >= K_F(1) && k <= K_F(10)) {
        static const int fkeys[10] = {
            CMD_HELP, CMD_USERMENU, CMD_VIEW, CMD_EDIT, CMD_COPY,
            CMD_MOVE, CMD_MKDIR, CMD_DELETE, CMD_MENU, CMD_QUIT
        };

        do_command(fkeys[k - K_F(1)], p);
        return;
    }

    switch (k) {
    case K_UP:          panel_move(p, -1); break;
    case K_DOWN:        panel_move(p, 1); break;
    case K_PGUP:        panel_move(p, -(rows - 1)); break;
    case K_PGDN:        panel_move(p, rows - 1); break;
    case K_HOME:        panel_goto(p, 0); break;
    case K_END:         panel_goto(p, p->n - 1); break;
    case K_CTRL_PGUP:   chdir_panel(p, ".."); break;
    case K_CTRL_PGDN:
        if (panel_current(p) && entry_is_dir(panel_current(p)))
            chdir_panel(p, panel_current(p)->name);
        break;
    case K_TAB:
        p->searching = 0;
        active = !active;
        break;
    case K_INS:
    case K_CTRL('t'):
        if (p->n > 0) {
            panel_mark(p, p->cur, !(p->e[p->cur].flags & E_MARKED));
            panel_move(p, 1);
        }
        break;
    case K_ENTER:
        if (cmdline[0] != '\0')
            execute_command_line();
        else
            open_current(p);
        break;
    case K_ALT | K_ENTER:       /* put the file name on the command line */
        if (panel_current(p) &&
            len + strlen(panel_current(p)->name) + 2 < sizeof cmdline) {
            strcat(cmdline, panel_current(p)->name);
            strcat(cmdline, " ");
        }
        break;
    case K_BS:
        while (len > 0 && (cmdline[--len] & 0xC0) == 0x80)
            ;
        cmdline[len] = '\0';
        break;
    case K_CTRL('r'):   panel_load(p); break;
    case K_CTRL('u'):   swap_panels(); break;
    case K_CTRL('o'):   run_show_output(); break;
    case K_CTRL('s'):
    case K_ALT | 's':   do_command(CMD_SEARCH, p); break;
    case K_CTRL('x'):   ctrl_x = 1; break;
    case K_CTRL('l'):   scr_invalidate(); break;
    case K_ALT | '.':   do_command(CMD_HIDDEN, p); break;
    case K_ALT | 'o':   do_command(CMD_OTHER_TO_CURSOR, p); break;
    case K_ALT | 'i':   do_command(CMD_OTHER_TO_HERE, p); break;
    case K_ESC:
        cmdline[0] = '\0';
        break;
    case K_MOUSE:
        handle_mouse();
        break;
    default:
        /* + - * act on marks only while the command line is empty */
        if (cmdline[0] == '\0' && (k == '+' || k == '-' || k == '*')) {
            do_command(k == '+' ? CMD_SELECT
                       : k == '-' ? CMD_UNSELECT : CMD_INVERT, p);
        } else if (k >= 32 && k < 256 && len + 1 < sizeof cmdline) {
            cmdline[len] = (char)k;
            cmdline[len + 1] = '\0';
        }
        break;
    }
}

/* ---- main ------------------------------------------------------------- */

int main(int argc, char **argv)
{
    char cwd[PATH_MAX];
    int i;

    if (argc > 1 && (strcmp(argv[1], "-h") == 0 ||
                     strcmp(argv[1], "--help") == 0)) {
        printf("usage: %s [left-dir [right-dir]]\n", argv[0]);
        return 0;
    }
    if (getcwd(cwd, sizeof cwd) == NULL)
        strcpy(cwd, "/");

    /* Panels start in the given directories (relative to cwd), else in
     * the current one. */
    for (i = 0; i < 2; i++) {
        char path[PATH_MAX];
        const char *arg = argc > 1 + i ? argv[1 + i] : ".";

        if (arg[0] == '/')
            snprintf(path, sizeof path, "%s", arg);
        else
            path_join(path, sizeof path, cwd, arg);
        panel_init(&panels[i], path);
        if (panel_load(&panels[i]) != 0) {
            fprintf(stderr, "mc: %s: %s\n", path, strerror(errno));
            return 1;
        }
    }

    if (scr_open() != 0) {
        fputs("mc: needs a terminal\n", stderr);
        return 1;
    }
    atexit(scr_close);

    while (!quit) {
        int k;

        app_draw();
        scr_flush();
        k = key_get(-1);
        if (k == K_QUIT)
            break;
        if (k == K_RESIZE)
            scr_update_size();
        else if (k != K_NONE)
            handle_key(k);
    }
    scr_close();
    panel_free(&panels[0]);
    panel_free(&panels[1]);
    return 0;
}
