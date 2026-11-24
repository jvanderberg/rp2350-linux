/*
 * usermenu.c - the F2 user menu. See usermenu.h for the file format.
 */
#include <fcntl.h>
#include <fnmatch.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dialog.h"
#include "panel.h"
#include "run.h"
#include "screen.h"
#include "usermenu.h"
#include "util.h"

/* Used when no menu file exists. Commands only use BusyBox tools. */
static const char default_menu[] =
    "# Default user menu. To change it, save your own as ~/.config/mc/menu,\n"
    "# or as .mc.menu in a directory to use it there only.\n"
    "shell_patterns=1\n"
    "\n"
    "+ f *.c\n"
    "c       Compile this file with tcc\n"
    "        tcc -Wall -o %b %f && echo \"built ./%b\"\n"
    "\n"
    "+ f *.c\n"
    "r       Compile and run this file\n"
    "        tcc -Wall -o %b %f && ./%b\n"
    "\n"
    "+ x build.sh\n"
    "b       Run ./build.sh here\n"
    "        sh ./build.sh\n"
    "\n"
    "+ f *.tar | f *.tar.gz | f *.tgz\n"
    "x       Extract this archive here\n"
    "        case %f in *.tar) tar xf %f ;; *) gunzip -c %f | tar xf - ;; esac\n"
    "\n"
    "+ f *.zip\n"
    "x       Extract this archive here\n"
    "        unzip %f\n"
    "\n"
    "+ f *.gz & ! f *.tar.gz\n"
    "u       Uncompress (keeps the .gz)\n"
    "        gunzip -c %f > %b\n"
    "\n"
    "+ t r & ! f *.gz\n"
    "z       Compress with gzip (keeps the original)\n"
    "        gzip -c %f > %f.gz\n"
    "\n"
    "+ t d & ! f ..\n"
    "a       Archive this directory into the other panel\n"
    "        tar cf - %f | gzip > %D/%f.tar.gz\n"
    "\n"
    "+ t r\n"
    "h       Hex dump\n"
    "        hexdump -C %f | less\n"
    "\n"
    "+ t rt\n"
    "m       MD5 checksums of the marked files\n"
    "        md5sum %s\n"
    "\n"
    "+ t rt\n"
    "w       Count lines, words and bytes\n"
    "        wc %s\n"
    "\n"
    "d       Disk usage of everything here\n"
    "        du -sk * .[!.]* 2>/dev/null | sort -n\n"
    "\n"
    "f       Find files by name...\n"
    "        find . -name '%{File name pattern, e.g. *.c:}'\n"
    "\n"
    "g       Search inside files (grep)...\n"
    "        grep -rn '%{Text to look for:}' .\n"
    "\n"
    "s       Copy to another machine (scp)...\n"
    "        scp -r %s '%{Destination, e.g. user@host:/path:}'\n";

struct uentry {
    char hotkey;
    char *label;            /* the entry line as shown in the menu */
    char *cmd;              /* its command lines, joined by '\n' */
    int visible, is_default;
};

/* ---- a growable string ------------------------------------------------ */

struct sbuf {
    char *s;
    size_t len, cap;
};

static void sb_add(struct sbuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->s = realloc(b->s, b->cap);
        if (b->s == NULL) {
            fputs("mc: out of memory\n", stderr);
            exit(1);
        }
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}

static void sb_str(struct sbuf *b, const char *s)
{
    sb_add(b, s, strlen(s));
}

/* Append a file name quoted for the shell (left bare when safe). */
static void sb_quoted(struct sbuf *b, const char *s)
{
    static const char safe[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
        "0123456789_-+.,/:=@%^";

    if (*s != '\0' && s[strspn(s, safe)] == '\0') {
        sb_str(b, s);
        return;
    }
    sb_add(b, "'", 1);
    for (; *s; s++) {
        if (*s == '\'')
            sb_str(b, "'\\''");
        else
            sb_add(b, s, 1);
    }
    sb_add(b, "'", 1);
}

/* ---- reading the menu ------------------------------------------------- */

static char *read_file(const char *path)
{
    struct stat st;
    char *buf;
    int fd = open(path, O_RDONLY);
    ssize_t n;

    if (fd < 0)
        return NULL;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 65536) {
        close(fd);
        return NULL;
    }
    buf = xmalloc((size_t)st.st_size + 1);
    n = read(fd, buf, (size_t)st.st_size);
    close(fd);
    buf[n > 0 ? n : 0] = '\0';
    return buf;
}

static char *load_menu(const char *dir)
{
    char path[PATH_MAX];
    const char *home = getenv("HOME");
    char *text;

    if (path_join(path, sizeof path, dir, ".mc.menu") == 0 &&
        (text = read_file(path)) != NULL)
        return text;
    if (home != NULL) {
        snprintf(path, sizeof path, "%s/.config/mc/menu", home);
        if ((text = read_file(path)) != NULL)
            return text;
        snprintf(path, sizeof path, "%s/.mc/menu", home);
        if ((text = read_file(path)) != NULL)
            return text;
    }
    return xstrdup(default_menu);
}

/* ---- conditions -------------------------------------------------------- */

static int match(const struct usermenu_ctx *c, const char *pattern, const char *s)
{
    regex_t re;
    int r;

    if (c->shell_patterns)
        return fnmatch(pattern, s, 0) == 0;
    if (regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB) != 0)
        return 0;
    r = regexec(&re, s, 0, NULL, 0) == 0;
    regfree(&re);
    return r;
}

/* Is panel p's current entry one of the types in `types`? */
static int type_is(struct panel *p, const char *types)
{
    struct entry *e = panel_current(p);

    for (; *types; types++) {
        switch (*types) {
        case 't': if (p->nmarked > 0) return 1; break;
        case 'r': if (e && S_ISREG(e->mode) && !(e->flags & E_LINK)) return 1;
                  break;
        case 'd': if (e && entry_is_dir(e)) return 1; break;
        case 'l': if (e && (e->flags & E_LINK)) return 1; break;
        case 'x': if (e && (e->flags & E_EXEC)) return 1; break;
        }
    }
    return 0;
}

static int test(const struct usermenu_ctx *c, char op, const char *arg)
{
    struct panel *p = op >= 'A' && op <= 'Z' ? c->other : c->p;
    struct entry *e = panel_current(p);
    char path[PATH_MAX];

    switch (op) {
    case 'f': case 'F':
        return e != NULL && match(c, arg, e->name);
    case 'd': case 'D':
        return match(c, arg, p->path);
    case 't': case 'T':
        return type_is(p, arg);
    case 'x':
        return path_join(path, sizeof path, c->p->path, arg) == 0 &&
               access(path, X_OK) == 0;
    }
    return 0;
}

/* Evaluate "f *.c & ! t d | x build.sh", left to right. */
static int eval_condition(const struct usermenu_ctx *c, const char *s)
{
    int value = -1, negate = 0;
    char join = '&';

    for (;;) {
        char op, arg[256];
        size_t n;
        int t;

        while (*s == ' ' || *s == '\t')
            s++;
        if (*s == '\0' || *s == '\n')
            break;
        if (*s == '&' || *s == '|') {
            join = *s++;
            continue;
        }
        if (*s == '!') {
            negate = !negate;
            s++;
            continue;
        }
        op = *s++;
        while (*s == ' ' || *s == '\t')
            s++;
        n = strcspn(s, " \t\n");
        if (n >= sizeof arg)
            n = sizeof arg - 1;
        memcpy(arg, s, n);
        arg[n] = '\0';
        s += n;

        t = test(c, op, arg);
        if (negate)
            t = !t;
        negate = 0;
        if (value < 0)
            value = t;
        else if (join == '|')
            value = value || t;
        else
            value = value && t;
    }
    return value != 0;
}

/* Split the menu text into entries, evaluating conditions as we go. */
static int parse_menu(char *text, struct usermenu_ctx *c, struct uentry **out)
{
    struct uentry *e = NULL;
    int n = 0, cap = 0, cond = 1, def = 0;
    struct sbuf cmd = { NULL, 0, 0 };
    char *line = text, *next;

    for (; line != NULL && *line; line = next) {
        next = strchr(line, '\n');
        if (next != NULL)
            *next++ = '\0';

        if (strncmp(line, "shell_patterns=", 15) == 0) {
            c->shell_patterns = atoi(line + 15);
        } else if (line[0] == '#' || line[0] == '\0') {
            continue;
        } else if (line[0] == '+' || line[0] == '=') {
            int is_def = line[0] == '=' || line[1] == '=';
            int ok = eval_condition(c, line + 1 + (line[1] == '+' ||
                                                   line[1] == '='));
            if (is_def)
                def = ok;
            if (line[0] == '+' || line[1] == '+')
                cond = ok;
        } else if (line[0] == ' ' || line[0] == '\t') {
            if (n > 0) {                /* a command line */
                while (*line == ' ' || *line == '\t')
                    line++;
                if (cmd.len > 0)
                    sb_add(&cmd, "\n", 1);
                sb_str(&cmd, line);
                free(e[n - 1].cmd);
                e[n - 1].cmd = xstrdup(cmd.s);
            }
        } else {                        /* a new entry */
            char label[256];
            int i, col = 0;

            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                e = realloc(e, sizeof *e * (size_t)cap);
                if (e == NULL)
                    exit(1);
            }
            for (i = 0; line[i] && col < (int)sizeof label - 9; i++) {
                if (line[i] == '\t')        /* expand tabs for display */
                    do
                        label[col++] = ' ';
                    while (col % 8 != 0);
                else
                    label[col++] = line[i];
            }
            label[col] = '\0';
            e[n].hotkey = line[0];
            e[n].label = xstrdup(label);
            e[n].cmd = xstrdup("");
            e[n].visible = cond;
            e[n].is_default = def && cond;
            n++;
            cmd.len = 0;
            cond = 1;
            def = 0;
        }
    }
    free(cmd.s);
    *out = e;
    return n;
}

/* ---- macros ----------------------------------------------------------- */

static void add_names(struct sbuf *b, struct panel *p, int marked_only)
{
    int i, first = 1;

    for (i = 0; i < p->n; i++)
        if (p->e[i].flags & E_MARKED) {
            if (!first)
                sb_add(b, " ", 1);
            sb_quoted(b, p->e[i].name);
            first = 0;
        }
    if (first && !marked_only && panel_current(p) != NULL)
        sb_quoted(b, panel_current(p)->name);
}

/* Expand the macros of `cmd`. Returns NULL if a %{prompt} was cancelled.
 * *unmark is set when %u was used. */
static char *expand(const struct usermenu_ctx *c, const char *cmd, int *unmark)
{
    struct sbuf b = { NULL, 0, 0 };
    struct entry *e;

    sb_add(&b, "", 0);
    for (; *cmd; cmd++) {
        struct panel *p;
        char m;

        if (*cmd != '%' || cmd[1] == '\0') {
            sb_add(&b, cmd, 1);
            continue;
        }
        m = *++cmd;
        p = m >= 'A' && m <= 'Z' ? c->other : c->p;
        e = panel_current(p);
        switch (m) {
        case '%':
            sb_add(&b, "%", 1);
            break;
        case 'f': case 'p': case 'F': case 'P':
            if (e != NULL)
                sb_quoted(&b, e->name);
            break;
        case 'b': case 'x':
            if (e != NULL) {
                const char *dot = strrchr(e->name, '.');
                char base[NAME_MAX + 1];

                if (dot == NULL || dot == e->name)
                    dot = e->name + strlen(e->name);
                if (m == 'x') {
                    sb_str(&b, *dot ? dot + 1 : "");
                } else {
                    snprintf(base, sizeof base, "%.*s",
                             (int)(dot - e->name), e->name);
                    sb_quoted(&b, base);
                }
            }
            break;
        case 'd': case 'D':
            sb_quoted(&b, p->path);
            break;
        case 's': case 'S':
            add_names(&b, p, 0);
            break;
        case 'u': case 'U':
            *unmark = 1;
            /* fall through */
        case 't': case 'T':
            add_names(&b, p, 1);
            break;
        case '{': {
            const char *end = strchr(cmd, '}');
            char prompt[128], answer[256] = "";

            if (end == NULL) {
                sb_str(&b, "%{");
                break;
            }
            snprintf(prompt, sizeof prompt, "%.*s", (int)(end - cmd - 1),
                     cmd + 1);
            if (!dlg_input_box("User menu", prompt, answer, sizeof answer,
                               50)) {
                free(b.s);
                return NULL;
            }
            sb_str(&b, answer);
            cmd = end;
            break;
        }
        default:
            sb_add(&b, cmd - 1, 2);
            break;
        }
    }
    return b.s;
}

/* ---- running ---------------------------------------------------------- */

int usermenu_open(struct usermenu *m, struct panel *p, struct panel *other)
{
    int i;

    memset(m, 0, sizeof *m);
    m->c.p = p;
    m->c.other = other;
    m->c.shell_patterns = 1;
    m->text = load_menu(p->path);
    m->ne = parse_menu(m->text, &m->c, &m->e);
    m->labels = xmalloc(sizeof *m->labels * (size_t)(m->ne + 1));
    m->hotkeys = xmalloc((size_t)m->ne + 1);
    m->map = xmalloc(sizeof *m->map * (size_t)(m->ne + 1));
    for (i = 0; i < m->ne; i++)
        if (m->e[i].visible) {
            if (m->e[i].is_default)
                m->def = m->n;
            m->labels[m->n] = m->e[i].label;
            m->hotkeys[m->n] = m->e[i].hotkey;
            m->map[m->n++] = i;
        }
    return m->n;
}

int usermenu_exec(struct usermenu *m, int i)
{
    char *script;
    int unmark = 0, ran = 0;

    if (i < 0 || i >= m->n)
        return 0;
    script = expand(&m->c, m->e[m->map[i]].cmd, &unmark);
    if (script != NULL && script[0] != '\0') {
        run_script(m->c.p->path, script);
        if (unmark)
            panel_unmark_all(m->c.p);
        ran = 1;
    }
    free(script);
    return ran;
}

void usermenu_close(struct usermenu *m)
{
    int i;

    for (i = 0; i < m->ne; i++) {
        free(m->e[i].label);
        free(m->e[i].cmd);
    }
    free(m->e);
    free(m->labels);
    free(m->hotkeys);
    free(m->map);
    free(m->text);
    memset(m, 0, sizeof *m);
}

int usermenu_run(struct panel *p, struct panel *other)
{
    struct usermenu m;
    int ran = 0;

    if (usermenu_open(&m, p, other) == 0) {
        dlg_message("User menu", 0, "No suitable entries found\n"
                    "in the user menu");
    } else {
        struct dialog d;
        struct widget *w;
        int width = 0, h = m.n, i;

        /* dlg_list(), but starting on the '=' default entry */
        for (i = 0; i < m.n; i++)
            if (utf8_len(m.labels[i]) > width)
                width = utf8_len(m.labels[i]);
        width += 8;
        if (width < 24)
            width = 24;
        if (h > scr_rows - 6)
            h = scr_rows - 6;
        dlg_init(&d, "User menu", h + 4, width, DLG_MENU);
        w = dlg_list_box(&d, 2, h, m.labels, m.hotkeys, m.n);
        w->pos = m.def;
        if (dlg_run(&d) == 0)
            ran = usermenu_exec(&m, w->pos);
    }
    usermenu_close(&m);
    return ran;
}
