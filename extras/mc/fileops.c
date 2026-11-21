/*
 * fileops.c - copy / move / delete / mkdir. See fileops.h.
 *
 * Everything is done in-process with plain system calls (no cp/mv/rm
 * child processes): directories are walked recursively, regular files
 * are copied through a small static buffer, symlinks are recreated, and
 * modes and modification times are preserved. A move is a rename() when
 * source and target are on the same filesystem, otherwise copy + delete.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "dialog.h"
#include "fileops.h"
#include "keys.h"
#include "screen.h"
#include "util.h"

/* Answers that apply to the rest of an operation. */
enum { ASK, ALL, NONE };

/* State of one running operation, including its progress box. */
struct job {
    const char *verb;           /* "Copy", "Move", "Delete" */
    int overwrite;              /* ASK / ALL / NONE for existing targets */
    int recurse;                /* ASK / ALL / NONE for non-empty dirs */
    int aborted;
    int errors;
    int files;                  /* files processed so far */
    long long last_draw;        /* to limit progress redraws */
    struct dialog dlg;
    struct widget *gauge;
    char line1[PATH_MAX + 16], line2[PATH_MAX + 16], line3[64];
};

static char copy_buf[16384];

/* ---- progress box ----------------------------------------------------- */

static void job_init(struct job *j, const char *verb, int with_target)
{
    static const char *const abort_btn[] = { "&Abort" };
    int w = scr_cols - 8 < 64 ? scr_cols - 8 : 64;

    memset(j, 0, sizeof *j);
    j->verb = verb;
    dlg_init(&j->dlg, verb, with_target ? 10 : 8, w, 0);
    dlg_label(&j->dlg, 2, j->line1, 0);
    if (with_target) {
        dlg_label(&j->dlg, 3, j->line2, 0);
        j->gauge = dlg_gauge(&j->dlg, 4);
    }
    dlg_label(&j->dlg, with_target ? 5 : 3, j->line3, 0);
    dlg_hline(&j->dlg, with_target ? 6 : 4);
    dlg_buttons(&j->dlg, with_target ? 7 : 5, 1, abort_btn, 1, 0);
    dlg_show(&j->dlg);
}

static void job_done(struct job *j)
{
    dlg_hide(&j->dlg);
}

/* Redraw the progress box (at most ~10 times a second unless forced) and
 * check whether the user wants to abort. */
static void progress(struct job *j, int force)
{
    long long t = now_us();
    int k;

    if (!force && t - j->last_draw < 100000)
        return;
    j->last_draw = t;
    snprintf(j->line3, sizeof j->line3, "Files processed: %d", j->files);
    dlg_redraw();
    while ((k = key_get(0)) != K_NONE) {
        if (k == K_RESIZE)
            scr_update_size();
        else if (k == K_ESC || k == K_ENTER || k == K_QUIT ||
                 k == 'a' || k == 'A' || k == (K_ALT | 'a') ||
                 (k == K_MOUSE && !key_mouse.release))
            j->aborted = 1;
    }
}

static void set_paths(struct job *j, const char *src, const char *dst)
{
    snprintf(j->line1, sizeof j->line1, "%s: %s",
             dst != NULL ? "Source" : "Deleting", src);
    if (dst != NULL)
        snprintf(j->line2, sizeof j->line2, "Target: %s", dst);
    if (j->gauge != NULL)
        j->gauge->value = 0;
}

/* Report a failed system call: Skip continues, Abort stops the job. */
static void fail(struct job *j, const char *what, const char *path)
{
    static const char *const btn[] = { "&Skip", "&Abort" };
    char msg[PATH_MAX + 128];
    int err = errno;

    j->errors++;
    snprintf(msg, sizeof msg, "%s \"%s\"\n%s", what, path, strerror(err));
    if (dlg_query("Error", 1, 0, msg, btn, 2) != 0)
        j->aborted = 1;
}

/* An existing target: may we replace it? */
static int may_overwrite(struct job *j, const char *dst)
{
    static const char *const btn[] = { "&Yes", "&No", "A&ll", "Non&e",
                                       "&Abort" };
    char msg[PATH_MAX + 64];
    int r;

    if (j->overwrite != ASK)
        return j->overwrite == ALL;
    snprintf(msg, sizeof msg, "Target file \"%s\" already exists!\n"
             "Overwrite it?", path_base(dst));
    r = dlg_query("File exists", 1, 0, msg, btn, 5);
    switch (r) {
    case 0: return 1;
    case 1: return 0;
    case 2: j->overwrite = ALL; return 1;
    case 3: j->overwrite = NONE; return 0;
    }
    j->aborted = 1;
    return 0;
}

/* Allocate "dir/name". */
static char *join(const char *dir, const char *name)
{
    size_t n = strlen(dir) + strlen(name) + 2;
    char *s = xmalloc(n);

    snprintf(s, n, "%s/%s", dir, name);
    return s;
}

static void keep_times(const char *path, const struct stat *st)
{
    struct timeval tv[2];

    tv[0].tv_sec = st->st_atime;
    tv[0].tv_usec = 0;
    tv[1].tv_sec = st->st_mtime;
    tv[1].tv_usec = 0;
    utimes(path, tv);
}

/* ---- delete ----------------------------------------------------------- */

static int dir_is_empty(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *de;
    int empty = 1;

    if (d == NULL)
        return 1;
    while (empty && (de = readdir(d)) != NULL)
        if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0)
            empty = 0;
    closedir(d);
    return empty;
}

/* Remove a file or a whole tree. `ask` makes a non-empty top-level
 * directory prompt first (nested ones were covered by that answer). */
static void delete_tree(struct job *j, const char *path, int ask)
{
    struct stat st;

    if (j->aborted)
        return;
    if (lstat(path, &st) != 0) {
        fail(j, "Cannot stat", path);
        return;
    }
    set_paths(j, path, NULL);
    progress(j, 0);

    if (S_ISDIR(st.st_mode)) {
        DIR *d;
        struct dirent *de;

        if (ask && !dir_is_empty(path)) {
            static const char *const btn[] = { "&Yes", "&No", "A&ll",
                                               "Non&e", "&Abort" };
            char msg[PATH_MAX + 64];
            int r = j->recurse == ALL ? 0 : j->recurse == NONE ? 1 : -1;

            if (r < 0) {
                snprintf(msg, sizeof msg, "Directory \"%s\" not empty.\n"
                         "Delete it recursively?", path_base(path));
                r = dlg_query("Delete", 1, 0, msg, btn, 5);
                if (r == 2) j->recurse = ALL;
                if (r == 3) j->recurse = NONE;
                if (r < 0 || r == 4) {
                    j->aborted = 1;
                    return;
                }
            }
            if (r == 1 || r == 3) {
                j->errors++;    /* skipped: keep it marked */
                return;
            }
        }
        d = opendir(path);
        if (d == NULL) {
            fail(j, "Cannot open directory", path);
            return;
        }
        while (!j->aborted && (de = readdir(d)) != NULL) {
            char *child;

            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            child = join(path, de->d_name);
            delete_tree(j, child, 0);
            free(child);
        }
        closedir(d);
        if (!j->aborted && rmdir(path) != 0)
            fail(j, "Cannot remove directory", path);
    } else if (unlink(path) != 0) {
        fail(j, "Cannot delete file", path);
    } else {
        j->files++;
    }
}

/* ---- copy ------------------------------------------------------------- */

static void copy_regular(struct job *j, const char *src, const char *dst,
                         const struct stat *st)
{
    int in, out;
    ssize_t n;
    long long done = 0;

    in = open(src, O_RDONLY);
    if (in < 0) {
        fail(j, "Cannot open source file", src);
        return;
    }
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st->st_mode & 07777);
    if (out < 0) {
        fail(j, "Cannot create target file", dst);
        close(in);
        return;
    }
    while (!j->aborted && (n = read(in, copy_buf, sizeof copy_buf)) != 0) {
        ssize_t off = 0;

        if (n < 0) {
            if (errno == EINTR)
                continue;
            fail(j, "Cannot read source file", src);
            break;
        }
        while (off < n) {
            ssize_t w = write(out, copy_buf + off, (size_t)(n - off));

            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0) {
                fail(j, "Cannot write target file", dst);
                j->aborted = 1;     /* a full disk won't get better */
                break;
            }
            off += w;
        }
        done += n;
        if (st->st_size > 0)
            j->gauge->value = (int)(done * 1000 / st->st_size);
        progress(j, 0);
    }
    close(in);
    if (close(out) != 0 && !j->aborted)
        fail(j, "Cannot close target file", dst);
    if (j->aborted) {
        unlink(dst);                /* don't leave half a file behind */
        return;
    }
    chmod(dst, st->st_mode & 07777);
    keep_times(dst, st);
    j->files++;
}

static void copy_tree(struct job *j, const char *src, const char *dst)
{
    struct stat st, dt;
    int exists;

    if (j->aborted)
        return;
    if (lstat(src, &st) != 0) {
        fail(j, "Cannot stat source", src);
        return;
    }
    set_paths(j, src, dst);
    progress(j, 0);
    exists = lstat(dst, &dt) == 0;
    if (exists && st.st_dev == dt.st_dev && st.st_ino == dt.st_ino) {
        errno = EINVAL;
        fail(j, "Cannot copy a file onto itself:", src);
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        DIR *d;
        struct dirent *de;

        if (path_inside(dst, src)) {
            errno = EINVAL;
            fail(j, "Cannot copy a directory into itself:", src);
            return;
        }
        if (exists && !S_ISDIR(dt.st_mode)) {
            errno = ENOTDIR;
            fail(j, "Cannot overwrite file with directory", dst);
            return;
        }
        if (!exists && mkdir(dst, (st.st_mode & 07777) | 0700) != 0) {
            fail(j, "Cannot create directory", dst);
            return;
        }
        d = opendir(src);
        if (d == NULL) {
            fail(j, "Cannot open directory", src);
            return;
        }
        while (!j->aborted && (de = readdir(d)) != NULL) {
            char *s, *t;

            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            s = join(src, de->d_name);
            t = join(dst, de->d_name);
            copy_tree(j, s, t);
            free(s);
            free(t);
        }
        closedir(d);
        chmod(dst, st.st_mode & 07777);
        keep_times(dst, &st);
        return;
    }

    if (exists) {
        if (S_ISDIR(dt.st_mode)) {
            errno = EISDIR;
            fail(j, "Cannot overwrite directory", dst);
            return;
        }
        if (!may_overwrite(j, dst))
            return;
        if (S_ISLNK(dt.st_mode) || !S_ISREG(st.st_mode))
            unlink(dst);
    }

    if (S_ISREG(st.st_mode)) {
        copy_regular(j, src, dst, &st);
    } else if (S_ISLNK(st.st_mode)) {
        char target[PATH_MAX];
        ssize_t n = readlink(src, target, sizeof target - 1);

        if (n < 0) {
            fail(j, "Cannot read link", src);
            return;
        }
        target[n] = '\0';
        if (symlink(target, dst) != 0)
            fail(j, "Cannot create link", dst);
        else
            j->files++;
    } else if (S_ISFIFO(st.st_mode)) {
        if (mkfifo(dst, st.st_mode & 07777) != 0)
            fail(j, "Cannot create fifo", dst);
        else
            j->files++;
    } else {
        errno = ENOTSUP;
        fail(j, "Cannot copy special file", src);
    }
}

static void move_tree(struct job *j, const char *src, const char *dst)
{
    struct stat st, dt;
    int errors = j->errors;

    if (j->aborted)
        return;
    set_paths(j, src, dst);
    progress(j, 0);
    if (lstat(src, &st) != 0) {
        fail(j, "Cannot stat source", src);
        return;
    }
    if (S_ISDIR(st.st_mode) && path_inside(dst, src)) {
        errno = EINVAL;
        fail(j, "Cannot move a directory into itself:", src);
        return;
    }
    if (lstat(dst, &dt) == 0) {
        if (st.st_dev == dt.st_dev && st.st_ino == dt.st_ino)
            return;                 /* same file: nothing to do */
        if (S_ISDIR(dt.st_mode) || S_ISDIR(st.st_mode)) {
            errno = EEXIST;
            fail(j, "Cannot move over existing", dst);
            return;
        }
        if (!may_overwrite(j, dst))
            return;
    }
    if (rename(src, dst) == 0) {
        j->files++;
        return;
    }
    if (errno != EXDEV) {
        fail(j, "Cannot move", src);
        return;
    }
    /* Different filesystem: copy, then delete the original if all of
     * the copy succeeded. */
    copy_tree(j, src, dst);
    if (!j->aborted && j->errors == errors) {
        j->recurse = ALL;
        delete_tree(j, src, 0);
    }
}

/* ---- the operations ---------------------------------------------------- */

int fop_copy(struct panel *sp, struct panel *dp, int move)
{
    const char *verb = move ? "Move" : "Copy";
    char dest[PATH_MAX], prompt[PATH_MAX + 64];
    int *idx, n, i, into_dir;
    size_t len;
    struct job j;

    n = panel_selection(sp, &idx);
    if (n == 0) {
        free(idx);
        return 0;
    }
    if (n == 1) {
        const struct entry *e = &sp->e[idx[0]];

        snprintf(prompt, sizeof prompt, "%s %s \"%s\" to:", verb,
                 S_ISDIR(e->mode) ? "directory" : "file", e->name);
    } else {
        snprintf(prompt, sizeof prompt, "%s %d files/directories to:",
                 verb, n);
    }
    snprintf(dest, sizeof dest, "%s%s", dp->path,
             strcmp(dp->path, "/") == 0 ? "" : "/");
    if (!dlg_input_box(verb, prompt, dest, sizeof dest, 68) ||
        dest[0] == '\0') {
        free(idx);
        return 0;
    }

    /* A relative target is relative to the source panel. */
    if (dest[0] != '/') {
        char tmp[PATH_MAX];

        if (path_join(tmp, sizeof tmp, sp->path, dest) == 0)
            strcpy(dest, tmp);
    }
    len = strlen(dest);
    into_dir = (len > 0 && dest[len - 1] == '/') || is_dir(dest);
    path_normalize(dest);
    if (n > 1 && !into_dir) {
        dlg_message("Error", 1, "Target \"%s\" is not a directory", dest);
        free(idx);
        return 0;
    }

    job_init(&j, verb, 1);
    for (i = 0; i < n && !j.aborted; i++) {
        const char *name = sp->e[idx[i]].name;
        char *src = join(sp->path, name);
        char *dst = into_dir ? join(dest, name) : xstrdup(dest);
        int errors = j.errors;

        path_normalize(src);
        path_normalize(dst);
        if (move)
            move_tree(&j, src, dst);
        else
            copy_tree(&j, src, dst);
        if (!j.aborted && j.errors == errors)
            panel_mark(sp, idx[i], 0);
        free(src);
        free(dst);
    }
    job_done(&j);
    free(idx);
    return 1;
}

int fop_delete(struct panel *p)
{
    static const char *const btn[] = { "&Yes", "&No" };
    char msg[PATH_MAX + 64];
    int *idx, n, i;
    struct job j;

    n = panel_selection(p, &idx);
    if (n == 0) {
        free(idx);
        return 0;
    }
    if (n == 1)
        snprintf(msg, sizeof msg, "Delete %s\n\"%s\"?",
                 S_ISDIR(p->e[idx[0]].mode) ? "directory" : "file",
                 p->e[idx[0]].name);
    else
        snprintf(msg, sizeof msg, "Delete %d files/directories?", n);
    if (dlg_query("Delete", 1, 0, msg, btn, 2) != 0) {
        free(idx);
        return 0;
    }

    job_init(&j, "Delete", 0);
    for (i = 0; i < n && !j.aborted; i++) {
        char *path = join(p->path, p->e[idx[i]].name);
        int errors = j.errors;

        delete_tree(&j, path, 1);
        if (!j.aborted && j.errors == errors)
            panel_mark(p, idx[i], 0);
        free(path);
    }
    job_done(&j);
    free(idx);
    return 1;
}

int fop_mkdir(struct panel *p)
{
    char name[NAME_MAX + 1] = "", path[PATH_MAX], *s;
    int idx;

    if (!dlg_input_box("Create a new Directory", "Enter directory name:",
                       name, sizeof name, 0) || name[0] == '\0')
        return 0;
    if (name[0] == '/')
        snprintf(path, sizeof path, "%s", name);
    else if (path_join(path, sizeof path, p->path, name) != 0)
        return 0;
    path_normalize(path);

    /* mkdir -p: create each missing component in turn. */
    for (s = path + 1;; s++) {
        if (*s == '/' || *s == '\0') {
            char c = *s;

            *s = '\0';
            if (mkdir(path, 0777) != 0 && errno != EEXIST) {
                dlg_message("Error", 1, "Cannot create directory\n\"%s\"\n%s",
                            path, strerror(errno));
                return 1;
            }
            *s = c;
            if (c == '\0')
                break;
        }
    }
    panel_load(p);
    /* Put the cursor on the new directory if it is in this panel. */
    s = strchr(name, '/');
    if (s != NULL)
        *s = '\0';
    idx = panel_find(p, name);
    if (idx >= 0)
        panel_goto(p, idx);
    return 1;
}
