/*
 * util.c - see util.h.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

#include "util.h"

void buf_add(struct buf *b, const void *data, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;

        while (cap < b->len + n + 1)
            cap *= 2;
        b->s = xrealloc(b->s, cap);
        b->cap = cap;
    }
    memcpy(b->s + b->len, data, n);
    b->len += n;
    b->s[b->len] = '\0';
}

void buf_str(struct buf *b, const char *s)
{
    buf_add(b, s, strlen(s));
}

void buf_printf(struct buf *b, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0)
        buf_add(b, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

void buf_free(struct buf *b)
{
    free(b->s);
    b->s = NULL;
    b->len = b->cap = 0;
}

int utf8_encode(char *out, uint32_t c)
{
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (char)(0xC0 | (c >> 6));
        out[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | (c >> 12));
        out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (c >> 18));
    out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3F));
    out[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

uint32_t utf8_decode(const unsigned char *s, size_t n, int *used)
{
    uint32_t c = s[0];
    int extra, i;

    if (c < 0x80) {
        *used = 1;
        return c;
    }
    extra = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2
          : (c & 0xF8) == 0xF0 ? 3 : -1;
    if (extra < 0) {
        *used = 1;
        return 0xFFFD;
    }
    if ((size_t)extra >= n) {
        for (i = 1; (size_t)i < n; i++)
            if ((s[i] & 0xC0) != 0x80) {
                *used = 1;
                return 0xFFFD;
            }
        *used = 0;                  /* incomplete: wait for more */
        return 0;
    }
    c &= 0x3F >> extra;
    for (i = 1; i <= extra; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *used = 1;
            return 0xFFFD;
        }
        c = (c << 6) | (s[i] & 0x3F);
    }
    *used = extra + 1;
    return c;
}

int char_width(uint32_t cp)
{
    int w;

    if (cp < 0x7F)
        return 1;
    w = wcwidth((wchar_t)cp);
    return w < 0 ? 1 : w > 2 ? 2 : w;
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);

    if (p == NULL) {
        fputs("tm: out of memory\n", stderr);
        exit(1);
    }
    return p;
}

void *xcalloc(size_t n, size_t size)
{
    void *p = calloc(n ? n : 1, size ? size : 1);

    if (p == NULL) {
        fputs("tm: out of memory\n", stderr);
        exit(1);
    }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (p == NULL) {
        fputs("tm: out of memory\n", stderr);
        exit(1);
    }
    return p;
}

long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
