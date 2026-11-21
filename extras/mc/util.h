/*
 * util.h - small helpers: UTF-8 decoding, number/date formatting, paths.
 */
#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <time.h>

/* Decode one UTF-8 character and advance *s past it. Malformed input
 * decodes to '?' so it can always be displayed. */
unsigned utf8_next(const char **s);

/* Number of characters (screen columns) in a UTF-8 string. */
int utf8_len(const char *s);

/* Size right-aligned in exactly `width` columns, switching to K/M/G
 * units when the plain number does not fit (the panel "Size" column). */
void fmt_size(char *out, int width, long long size);

/* Size in at most 4 digits plus a unit: "2077M", "926G" (free space). */
void fmt_short(char *out, long long bytes);

/* 1234567 -> "1,234,567". */
void fmt_commas(char *out, unsigned long long v);

/* mc's 12-column date: "Nov 16 22:55", or "Nov 16  2025" if old. */
void fmt_time(char *out, time_t t);

/* out = dir + "/" + name. Returns -1 (and leaves out empty) if it does
 * not fit in cap bytes. */
int path_join(char *out, size_t cap, const char *dir, const char *name);

/* Collapse "//", "." and ".." in an absolute path, in place. */
void path_normalize(char *path);

/* 1 if `path` is `dir` itself or somewhere below it. */
int path_inside(const char *path, const char *dir);

/* The last component of a path. */
const char *path_base(const char *path);

/* 1 if path names a directory (following symlinks). */
int is_dir(const char *path);

void *xmalloc(size_t n);
char *xstrdup(const char *s);

/* Monotonic time in microseconds. */
long long now_us(void);

#endif /* UTIL_H */
