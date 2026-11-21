/*
 * util.c - small helpers shared by all modules. See util.h.
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "util.h"

unsigned utf8_next(const char **ps)
{
    const unsigned char *s = (const unsigned char *)*ps;
    unsigned c = *s++;
    int extra;

    if (c < 0x80)
        extra = 0;
    else if ((c & 0xE0) == 0xC0) {
        c &= 0x1F;
        extra = 1;
    } else if ((c & 0xF0) == 0xE0) {
        c &= 0x0F;
        extra = 2;
    } else if ((c & 0xF8) == 0xF0) {
        c &= 0x07;
        extra = 3;
    } else {
        *ps = (const char *)s;
        return '?';
    }
    while (extra-- > 0) {
        if ((*s & 0xC0) != 0x80) {
            *ps = (const char *)s;
            return '?';
        }
        c = (c << 6) | (*s++ & 0x3F);
    }
    *ps = (const char *)s;
    return c;
}

int utf8_len(const char *s)
{
    int n = 0;

    for (; *s; s++)
        if ((*s & 0xC0) != 0x80)
            n++;
    return n;
}

void fmt_size(char *out, int width, long long size)
{
    static const char units[] = "KMGTP";
    int u = -1;

    if (size < 0)
        size = 0;
    snprintf(out, 32, "%*lld", width, size);
    while ((int)strlen(out) > width && u < 4) {
        size = (size + 1023) / 1024;
        u++;
        snprintf(out, 32, "%*lld%c", width - 1, size, units[u]);
    }
}

void fmt_short(char *out, long long bytes)
{
    static const char *units[] = { "", "K", "M", "G", "T", "P" };
    int u = 0;

    while (bytes >= 10000 && u < 5) {
        bytes /= 1024;
        u++;
    }
    sprintf(out, "%lld%s", bytes, units[u]);
}

void fmt_commas(char *out, unsigned long long v)
{
    char tmp[32];
    int len, i, o = 0;

    len = sprintf(tmp, "%llu", v);
    for (i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0)
            out[o++] = ',';
        out[o++] = tmp[i];
    }
    out[o] = '\0';
}

void fmt_time(char *out, time_t t)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    time_t now = time(NULL);
    struct tm *tm = localtime(&t);

    if (tm == NULL) {
        strcpy(out, "            ");
        return;
    }
    /* Like ls -l: show the time for the last six months, else the year. */
    if (now - t > 6L * 30 * 24 * 3600 || t - now > 3600)
        sprintf(out, "%.3s %2d  %4d", months + 3 * tm->tm_mon, tm->tm_mday,
                tm->tm_year + 1900);
    else
        sprintf(out, "%.3s %2d %02d:%02d", months + 3 * tm->tm_mon,
                tm->tm_mday, tm->tm_hour, tm->tm_min);
}

int path_join(char *out, size_t cap, const char *dir, const char *name)
{
    size_t dl = strlen(dir), nl = strlen(name);
    int slash = dl > 0 && dir[dl - 1] != '/';

    if (dl + slash + nl + 1 > cap) {
        out[0] = '\0';
        return -1;
    }
    memmove(out, dir, dl);
    if (slash)
        out[dl++] = '/';
    memcpy(out + dl, name, nl + 1);
    return 0;
}

void path_normalize(char *path)
{
    char out[PATH_MAX];
    size_t len = 0;
    const char *p = path;

    while (*p) {
        const char *start;
        size_t n;

        while (*p == '/')
            p++;
        start = p;
        while (*p && *p != '/')
            p++;
        n = (size_t)(p - start);
        if (n == 0 || (n == 1 && start[0] == '.'))
            continue;
        if (n == 2 && start[0] == '.' && start[1] == '.') {
            while (len > 0 && out[len - 1] != '/')
                len--;
            if (len > 0)
                len--;          /* drop the slash too */
            continue;
        }
        if (len + 1 + n >= sizeof out)
            break;
        out[len++] = '/';
        memcpy(out + len, start, n);
        len += n;
    }
    if (len == 0)
        out[len++] = '/';
    out[len] = '\0';
    strcpy(path, out);
}

int path_inside(const char *path, const char *dir)
{
    size_t n = strlen(dir);

    if (n == 1 && dir[0] == '/')
        return 1;
    return strncmp(path, dir, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

const char *path_base(const char *path)
{
    const char *s = strrchr(path, '/');

    return s != NULL && s[1] != '\0' ? s + 1 : path;
}

int is_dir(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);

    if (p == NULL) {
        fputs("mc: out of memory\n", stderr);
        exit(1);
    }
    return p;
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;

    return memcpy(xmalloc(n), s, n);
}

long long now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
