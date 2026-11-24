/*
 * panel.h - one directory panel: listing, sorting, cursor, marks, drawing.
 *
 * A panel occupies a rectangle of the screen and draws itself in mc's
 * "full" listing format:
 *
 *   ┌<─ /path ─────────.[^]>┐   frame with the directory as its title
 *   │.n   Name    │ Size  │Modify time │   column headers
 *   │/..          │UP--DIR│Nov 16 22:55│   one row per entry
 *   ├──── 12 B in 1 file ───┤   separator (+ total of marked files)
 *   │ name-of-current-file  │   mini status line
 *   └──────── 9G / 14G (64%) ─┘   free space on the filesystem
 */
#ifndef PANEL_H
#define PANEL_H

#include <limits.h>
#include <sys/types.h>
#include <time.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* entry.flags */
#define E_MARKED   1    /* tagged with Insert / Ctrl-T */
#define E_LINK     2    /* a symbolic link */
#define E_LINKDIR  4    /* a symbolic link to a directory */
#define E_STALE    8    /* a symbolic link to nothing */
#define E_UPDIR    16   /* the ".." entry */
#define E_EXEC     32   /* shown as executable (see panel.c) */

struct entry {
    char *name;
    long long size;
    time_t mtime;
    mode_t mode;
    unsigned char flags;
};

enum sort_key { SORT_NAME, SORT_EXT, SORT_SIZE, SORT_MTIME };

struct panel {
    char path[PATH_MAX];    /* absolute, normalized directory */
    struct entry *e;        /* entries, sorted; ".." first if present */
    int n, cap;
    int cur, top;           /* cursor index, first visible index */
    int sort, reverse;      /* sort key (enum sort_key), descending? */
    int nmarked;            /* number of entries with E_MARKED */
    long long marked_bytes; /* total size of marked entries */
    long long fs_free, fs_total;  /* free/total bytes, -1 unknown */
    int searching;          /* quick search (Ctrl-S) active */
    char search[64];
    int x, y, w, h;         /* screen rectangle, set by the layout */
};

/* Global listing options (Options menu). */
extern int opt_show_hidden;     /* show dot files (mc default: on) */
extern int opt_mix_files;       /* don't group directories first */

void panel_init(struct panel *p, const char *path);
void panel_free(struct panel *p);

/* Re-read the directory, keeping the cursor on the same name and the
 * marks on the same files. Returns -1 if the directory can't be read. */
int panel_load(struct panel *p);

/* Change directory; `dir` may be absolute or relative to the panel (e.g.
 * ".."). When going up, the cursor lands on the directory we came from.
 * Returns -1 (panel unchanged, errno set) on failure. */
int panel_chdir(struct panel *p, const char *dir);

void panel_draw(const struct panel *p, int active);

int  panel_list_rows(const struct panel *p);  /* visible entry rows */
void panel_move(struct panel *p, int delta);  /* move the cursor */
void panel_goto(struct panel *p, int idx);    /* put cursor on index */
int  panel_find(const struct panel *p, const char *name);  /* -1 if none */
int  panel_hit(const struct panel *p, int y, int x);  /* mouse -> index */
struct entry *panel_current(struct panel *p);

/* Marking. Patterns use shell wildcards (fnmatch). */
void panel_mark(struct panel *p, int idx, int on);
void panel_mark_glob(struct panel *p, const char *pattern, int on);
void panel_invert(struct panel *p);
void panel_unmark_all(struct panel *p);

/* The entries an operation applies to: all marked ones, or else the one
 * under the cursor (never ".."). Stores a malloc'd index array in *idx
 * (caller frees) and returns the count, which may be 0. */
int panel_selection(struct panel *p, int **idx);

/* Quick search: move to the next entry at or after `from` whose name
 * starts with p->search. Returns 0 if there is none. */
int panel_search(struct panel *p, int from);

int entry_is_dir(const struct entry *e);

/* The colour mc shows the entry's name in (an FG_* value from skin.h). */
int entry_color(const struct entry *e);

#endif /* PANEL_H */
