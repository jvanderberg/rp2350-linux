/*
 * util.h - growable byte buffers, UTF-8, character widths, allocation.
 */
#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <stdint.h>

/* A growable byte buffer. Start with all fields zero. */
struct buf {
    char *s;
    size_t len, cap;
};

void buf_add(struct buf *b, const void *data, size_t n);
void buf_str(struct buf *b, const char *s);
void buf_printf(struct buf *b, const char *fmt, ...);
void buf_free(struct buf *b);

/* Encode a code point as UTF-8 into out (4 bytes); returns the length. */
int utf8_encode(char *out, uint32_t cp);

/* Decode UTF-8 from s[0, n); sets *used to the bytes consumed (1 for a
 * malformed byte, which decodes to U+FFFD). Returns 0 with *used 0 if the
 * sequence is cut off at the end of the input. */
uint32_t utf8_decode(const unsigned char *s, size_t n, int *used);

/* Screen columns of a code point: 0 (combining), 1 or 2. */
int char_width(uint32_t cp);

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);

/* Monotonic milliseconds. */
long long now_ms(void);

#endif /* UTIL_H */
