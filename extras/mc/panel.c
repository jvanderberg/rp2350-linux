/*
 * panel.c - directory panels. See panel.h for the layout.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/vfs.h>
#endif

#include "panel.h"
#include "screen.h"
#include "skin.h"
#include "util.h"

int opt_show_hidden = 1;
int opt_mix_files;

/* ---- file type colours ------------------------------------------------ */

/* Extension lists from mc's filehighlight.ini (abridged). */
static const char ext_archive[] =
    ";7z;ace;arc;arj;bz2;cab;cpio;deb;gz;jar;lha;lz;lz4;lzh;lzma;lzo;rar;"
    "rpm;tar;taz;tbz;tbz2;tgz;tlz;txz;tz;xz;z;zip;zoo;zst;";
static const char ext_doc[] =
    ";chm;css;diz;doc;docx;dtd;htm;html;lsm;md;nfo;odp;ods;odt;pdf;php;"
    "pps;ppt;pptx;ps;rtf;sgml;sql;tex;txt;xls;xlsx;xml;";
static const char ext_source[] =
    ";ada;asm;awk;bash;c;cc;cgi;cpp;cxx;diff;erl;go;h;hh;hpp;hs;inc;java;"
    "js;lua;m4;mak;mjs;ml;pas;patch;pio;pl;pm;py;rb;rs;s;sh;tcl;ts;zsh;";
static const char ext_media[] =
    ";3gp;aac;ac3;avi;flac;flv;m4a;mid;midi;mkv;mod;mov;mp2;mp3;mp4;mpeg;"
    "mpg;ogg;ogv;opus;s3m;wav;webm;wma;wmv;xm;";
static const char ext_graph[] =
    ";bmp;gif;ico;jpeg;jpg;pbm;pcx;pgm;png;ppm;psd;svg;tga;tif;tiff;webp;"
    "xbm;xcf;xpm;";
static const char ext_database[] = ";db;dbf;mdb;mdn;sqlite;";
static const char ext_temp[] = ";bak;old;swp;tmp;";

/* Lower-cased extension of `name` (empty if none) wrapped as ";ext;". */
static void ext_key(const char *name, char *key, size_t cap)
{
    const char *dot = strrchr(name, '.');
    size_t i = 0;

    key[i++] = ';';
    if (dot != NULL && dot != name)
        for (dot++; *dot && i < cap - 2; dot++)
            key[i++] = (char)tolower((unsigned char)*dot);
    key[i++] = ';';
    key[i] = '\0';
}

/* Foreground colour for an entry, in mc's filehighlight rule order. */
int entry_color(const struct entry *e)
{
    char key[20];
    size_t len;

    if (e->flags & E_UPDIR || S_ISDIR(e->mode) || e->flags & E_LINKDIR)
        return FG_DIR;
    if (e->flags & E_STALE)
        return FG_STALE;
    if (S_ISCHR(e->mode) || S_ISBLK(e->mode))
        return FG_DEVICE;
    if (S_ISFIFO(e->mode) || S_ISSOCK(e->mode))
        return FG_SPECIAL;
    if (e->flags & E_EXEC)
        return FG_EXEC;
    if (e->flags & E_LINK)
        return FG_LINK;
    if (strcmp(e->name, "core") == 0)
        return FG_CORE;
    len = strlen(e->name);
    if (len > 0 && e->name[len - 1] == '~')
        return FG_TEMP;
    ext_key(e->name, key, sizeof key);
    if (strcmp(key, ";;") == 0)
        return C_LIGHTGRAY;
    if (strstr(ext_temp, key)) return FG_TEMP;
    if (strstr(ext_archive, key)) return FG_ARCHIVE;
    if (strstr(ext_doc, key)) return FG_DOC;
    if (strstr(ext_source, key)) return FG_SOURCE;
    if (strstr(ext_media, key)) return FG_MEDIA;
    if (strstr(ext_graph, key)) return FG_GRAPH;
    if (strstr(ext_database, key)) return FG_DATABASE;
    return C_LIGHTGRAY;
}

int entry_is_dir(const struct entry *e)
{
    return (e->flags & (E_UPDIR | E_LINKDIR)) || S_ISDIR(e->mode);
}

/* ---- sorting ---------------------------------------------------------- */

/* qsort() has no context argument, so the active settings live here. */
static int sort_key, sort_reverse;

static const char *ext_of(const char *name)
{
    const char *dot = strrchr(name, '.');

    return dot != NULL && dot != name ? dot + 1 : "";
}

static int cmp_entries(const void *pa, const void *pb)
{
    const struct entry *a = pa, *b = pb;
    int r = 0;

    if (a->flags & E_UPDIR)
        return -1;
    if (b->flags & E_UPDIR)
        return 1;
    if (!opt_mix_files && entry_is_dir(a) != entry_is_dir(b))
        return entry_is_dir(a) ? -1 : 1;

    switch (sort_key) {
    case SORT_EXT:
        r = strcmp(ext_of(a->name), ext_of(b->name));
        break;
    case SORT_SIZE:     /* mc lists the biggest first */
        r = a->size < b->size ? 1 : a->size > b->size ? -1 : 0;
        break;
    case SORT_MTIME:
        r = a->mtime < b->mtime ? -1 : a->mtime > b->mtime ? 1 : 0;
        break;
    }
    if (r == 0)
        r = strcmp(a->name, b->name);
    return sort_reverse ? -r : r;
}

static void panel_sort(struct panel *p)
{
    sort_key = p->sort;
    sort_reverse = p->reverse;
    qsort(p->e, (size_t)p->n, sizeof *p->e, cmp_entries);
}

/* ---- loading ---------------------------------------------------------- */

void panel_init(struct panel *p, const char *path)
{
    memset(p, 0, sizeof *p);
    snprintf(p->path, sizeof p->path, "%s", path);
    path_normalize(p->path);
    p->fs_free = p->fs_total = -1;
}

static void free_entries(struct entry *e, int n)
{
    int i;

    for (i = 0; i < n; i++)
        free(e[i].name);
    free(e);
}

void panel_free(struct panel *p)
{
    free_entries(p->e, p->n);
    p->e = NULL;
    p->n = p->cap = 0;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Is the directory on a FAT filesystem? FAT has no permission bits, so
 * every file there looks executable; we then only believe the x bit for
 * names without an extension (programs, not "notes.txt"). */
static int on_fat(const char *path)
{
#ifdef __linux__
    struct statfs sf;

    return statfs(path, &sf) == 0 && sf.f_type == 0x4d44;  /* MSDOS */
#else
    (void)path;
    return 0;
#endif
}

int panel_load(struct panel *p)
{
    DIR *d;
    struct dirent *de;
    struct statvfs vfs;
    struct entry *old = p->e;
    int oldn = p->n, i, nmarks = 0, fat;
    char **marks = NULL, *cur_name = NULL;

    d = opendir(p->path);
    if (d == NULL)
        return -1;
    fat = on_fat(p->path);

    /* Remember the cursor and the marks by name, to restore them. */
    if (oldn > 0) {
        cur_name = p->cur < oldn ? old[p->cur].name : NULL;
        marks = xmalloc(sizeof *marks * (size_t)oldn);
        for (i = 0; i < oldn; i++)
            if (old[i].flags & E_MARKED)
                marks[nmarks++] = old[i].name;
        qsort(marks, (size_t)nmarks, sizeof *marks, cmp_str);
    }

    p->e = NULL;
    p->n = p->cap = 0;
    p->nmarked = 0;
    p->marked_bytes = 0;

    while ((de = readdir(d)) != NULL) {
        struct entry *e;
        struct stat st;
        const char *name = de->d_name;

        if (strcmp(name, ".") == 0)
            continue;
        if (strcmp(name, "..") == 0) {
            if (strcmp(p->path, "/") == 0)
                continue;
        } else if (name[0] == '.' && !opt_show_hidden) {
            continue;
        }
        if (p->n == p->cap) {
            p->cap = p->cap ? p->cap * 2 : 64;
            p->e = realloc(p->e, sizeof *p->e * (size_t)p->cap);
            if (p->e == NULL) {
                fputs("mc: out of memory\n", stderr);
                exit(1);
            }
        }
        e = &p->e[p->n++];
        memset(e, 0, sizeof *e);
        e->name = xstrdup(name);
        if (strcmp(name, "..") == 0)
            e->flags |= E_UPDIR;
        if (fstatat(dirfd(d), name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
            e->mode = st.st_mode;
            e->size = st.st_size;
            e->mtime = st.st_mtime;
            if (S_ISLNK(st.st_mode)) {
                struct stat tgt;

                e->flags |= E_LINK;
                if (fstatat(dirfd(d), name, &tgt, 0) != 0)
                    e->flags |= E_STALE;
                else if (S_ISDIR(tgt.st_mode))
                    e->flags |= E_LINKDIR;
            }
            if (S_ISREG(st.st_mode) && (st.st_mode & 0111) &&
                (!fat || strchr(name + 1, '.') == NULL))
                e->flags |= E_EXEC;
        }
        if (nmarks > 0 && !(e->flags & E_UPDIR) &&
            bsearch(&e->name, marks, (size_t)nmarks, sizeof *marks, cmp_str)) {
            e->flags |= E_MARKED;
            p->nmarked++;
            p->marked_bytes += e->size;
        }
    }
    closedir(d);

    panel_sort(p);
    if (statvfs(p->path, &vfs) == 0) {
        p->fs_free = (long long)vfs.f_bavail * (long long)vfs.f_frsize;
        p->fs_total = (long long)vfs.f_blocks * (long long)vfs.f_frsize;
    } else {
        p->fs_free = p->fs_total = -1;
    }

    i = cur_name != NULL ? panel_find(p, cur_name) : -1;
    panel_goto(p, i >= 0 ? i : p->cur);

    free(marks);
    free_entries(old, oldn);
    return 0;
}

int panel_chdir(struct panel *p, const char *dir)
{
    char old[PATH_MAX], target[PATH_MAX];
    struct entry *saved_e = p->e;
    int saved_n = p->n, saved_cap = p->cap, idx;
    size_t n;

    if (dir[0] == '/')
        snprintf(target, sizeof target, "%s", dir);
    else if (path_join(target, sizeof target, p->path, dir) != 0) {
        errno = ENAMETOOLONG;
        return -1;
    }
    path_normalize(target);
    if (access(target, X_OK) != 0)
        return -1;

    strcpy(old, p->path);
    strcpy(p->path, target);
    /* A fresh listing: drop the old entries (and their marks). */
    p->e = NULL;
    p->n = p->cap = 0;
    p->cur = p->top = 0;
    p->searching = 0;
    if (panel_load(p) != 0) {
        int err = errno;

        strcpy(p->path, old);
        p->e = saved_e;
        p->n = saved_n;
        p->cap = saved_cap;
        errno = err;
        return -1;
    }
    free_entries(saved_e, saved_n);

    /* Going up: put the cursor on the directory we just left. */
    n = strlen(target);
    if (path_inside(old, target) && strcmp(old, target) != 0) {
        char child[NAME_MAX + 1];
        const char *s = old + n + (n > 1);
        size_t len = strcspn(s, "/");

        if (len < sizeof child) {
            memcpy(child, s, len);
            child[len] = '\0';
            idx = panel_find(p, child);
            if (idx >= 0)
                panel_goto(p, idx);
        }
    }
    return 0;
}

/* ---- cursor and marks ------------------------------------------------- */

int panel_list_rows(const struct panel *p)
{
    return p->h - 5 > 1 ? p->h - 5 : 1;
}

void panel_goto(struct panel *p, int idx)
{
    int rows = panel_list_rows(p);

    if (idx >= p->n)
        idx = p->n - 1;
    if (idx < 0)
        idx = 0;
    p->cur = idx;
    if (p->cur < p->top)
        p->top = p->cur;
    if (p->cur >= p->top + rows)
        p->top = p->cur - rows + 1;
    if (p->top > p->n - rows)
        p->top = p->n - rows;
    if (p->top < 0)
        p->top = 0;
}

void panel_move(struct panel *p, int delta)
{
    panel_goto(p, p->cur + delta);
}

int panel_find(const struct panel *p, const char *name)
{
    int i;

    for (i = 0; i < p->n; i++)
        if (strcmp(p->e[i].name, name) == 0)
            return i;
    return -1;
}

struct entry *panel_current(struct panel *p)
{
    return p->n > 0 ? &p->e[p->cur] : NULL;
}

int panel_hit(const struct panel *p, int y, int x)
{
    int row = y - (p->y + 2), idx;

    if (x <= p->x || x >= p->x + p->w - 1)
        return -1;
    if (row < 0 || row >= panel_list_rows(p))
        return -1;
    idx = p->top + row;
    return idx < p->n ? idx : -1;
}

void panel_mark(struct panel *p, int idx, int on)
{
    struct entry *e;

    if (idx < 0 || idx >= p->n)
        return;
    e = &p->e[idx];
    if ((e->flags & E_UPDIR) || !!(e->flags & E_MARKED) == !!on)
        return;
    if (on) {
        e->flags |= E_MARKED;
        p->nmarked++;
        p->marked_bytes += e->size;
    } else {
        e->flags &= ~E_MARKED;
        p->nmarked--;
        p->marked_bytes -= e->size;
    }
}

void panel_mark_glob(struct panel *p, const char *pattern, int on)
{
    int i;

    for (i = 0; i < p->n; i++)
        if (fnmatch(pattern, p->e[i].name, 0) == 0)
            panel_mark(p, i, on);
}

/* Like mc, invert only files; directories keep their state. */
void panel_invert(struct panel *p)
{
    int i;

    for (i = 0; i < p->n; i++)
        if (!entry_is_dir(&p->e[i]))
            panel_mark(p, i, !(p->e[i].flags & E_MARKED));
}

void panel_unmark_all(struct panel *p)
{
    int i;

    for (i = 0; i < p->n; i++)
        panel_mark(p, i, 0);
}

int panel_selection(struct panel *p, int **idx)
{
    int i, n = 0;

    *idx = xmalloc(sizeof **idx * (size_t)(p->n + 1));
    for (i = 0; i < p->n; i++)
        if (p->e[i].flags & E_MARKED)
            (*idx)[n++] = i;
    if (n == 0 && p->n > 0 && !(p->e[p->cur].flags & E_UPDIR))
        (*idx)[n++] = p->cur;
    return n;
}

int panel_search(struct panel *p, int from)
{
    size_t len = strlen(p->search);
    int pass, k, i;

    /* exact case first, then ignoring case */
    for (pass = 0; pass < 2; pass++)
        for (k = 0; k < p->n; k++) {
            i = (from + k) % p->n;
            if ((pass == 0 ? strncmp : strncasecmp)(p->e[i].name, p->search,
                                                    len) == 0) {
                panel_goto(p, i);
                return 1;
            }
        }
    return 0;
}
