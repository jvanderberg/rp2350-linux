/*
 * lz.c - LZ4-format block compressor. See lz.h.
 *
 * A compressed block is a series of sequences:
 *
 *   token          high nibble: literal count, low nibble: match length - 4
 *                  (15 in either means "more length bytes follow")
 *   [length bytes] each adds 0..255; a byte < 255 ends the run
 *   literals       copied as they are
 *   offset         2 bytes, little-endian: how far back the match starts
 *   [length bytes] for the match length
 *
 * The last sequence has only literals: the block ends after them.
 */
#include <string.h>

#include "lz.h"

#define MIN_MATCH   4
#define HASH_BITS   12
#define HASH_SIZE   (1 << HASH_BITS)
#define TAIL        5       /* keep the last bytes as literals (LZ4 rule) */

static unsigned hash4(const unsigned char *p)
{
    unsigned v = (unsigned)p[0] | (unsigned)p[1] << 8 |
                 (unsigned)p[2] << 16 | (unsigned)p[3] << 24;

    return (v * 2654435761u) >> (32 - HASH_BITS);
}

/* Write a length extension (the part of a length beyond 15). */
static unsigned char *put_len(unsigned char *o, const unsigned char *end,
                              size_t len)
{
    while (len >= 255) {
        if (o >= end)
            return NULL;
        *o++ = 255;
        len -= 255;
    }
    if (o >= end)
        return NULL;
    *o++ = (unsigned char)len;
    return o;
}

/* Emit literals [lit, lit + nlit) followed by a match (mlen 0: none). */
static unsigned char *put_seq(unsigned char *o, const unsigned char *end,
                              const unsigned char *lit, size_t nlit,
                              size_t offset, size_t mlen)
{
    unsigned char *token;
    size_t ml = mlen ? mlen - MIN_MATCH : 0;

    if (o >= end)
        return NULL;
    token = o++;
    *token = (unsigned char)((nlit < 15 ? nlit : 15) << 4 |
                             (ml < 15 ? ml : 15));
    if (nlit >= 15 && (o = put_len(o, end, nlit - 15)) == NULL)
        return NULL;
    if ((size_t)(end - o) < nlit)
        return NULL;
    memcpy(o, lit, nlit);
    o += nlit;
    if (mlen == 0)
        return o;
    if (end - o < 2)
        return NULL;
    *o++ = (unsigned char)(offset & 0xFF);
    *o++ = (unsigned char)(offset >> 8);
    if (ml >= 15 && (o = put_len(o, end, ml - 15)) == NULL)
        return NULL;
    return o;
}

size_t lz_compress(const unsigned char *src, size_t n, unsigned char *dst,
                   size_t cap)
{
    static unsigned short table[HASH_SIZE];     /* position + 1, 0 = none */
    const unsigned char *end = dst + (cap < n ? cap : n);
    unsigned char *o = dst;
    size_t i = 0, anchor = 0;

    if (n > LZ_MAX_BLOCK || n == 0)
        return 0;
    memset(table, 0, sizeof table);
    while (n >= MIN_MATCH + TAIL && i + MIN_MATCH + TAIL <= n) {
        unsigned h = hash4(src + i);
        size_t cand = table[h], len;

        table[h] = (unsigned short)(i + 1);
        if (cand == 0 || memcmp(src + cand - 1, src + i, MIN_MATCH) != 0) {
            i++;
            continue;
        }
        cand--;
        len = MIN_MATCH;
        while (i + len < n - TAIL && src[cand + len] == src[i + len])
            len++;
        o = put_seq(o, end, src + anchor, i - anchor, i - cand, len);
        if (o == NULL)
            return 0;
        i += len;
        anchor = i;
    }
    o = put_seq(o, end, src + anchor, n - anchor, 0, 0);
    if (o == NULL || (size_t)(o - dst) >= n)
        return 0;
    return (size_t)(o - dst);
}

/* Read a length extension; returns 0 on overrun. */
static int get_len(const unsigned char **p, const unsigned char *end,
                   size_t *len)
{
    unsigned char b;

    do {
        if (*p >= end)
            return 0;
        b = *(*p)++;
        *len += b;
    } while (b == 255);
    return 1;
}

size_t lz_decompress(const unsigned char *src, size_t n, unsigned char *dst,
                     size_t cap)
{
    const unsigned char *p = src, *end = src + n;
    unsigned char *o = dst, *oend = dst + cap;

    while (p < end) {
        unsigned token = *p++;
        size_t lit = token >> 4, mlen = token & 15, off;

        if (lit == 15 && !get_len(&p, end, &lit))
            return 0;
        if ((size_t)(end - p) < lit || (size_t)(oend - o) < lit)
            return 0;
        memcpy(o, p, lit);
        o += lit;
        p += lit;
        if (p == end)
            break;                  /* the final, literals-only sequence */
        if (end - p < 2)
            return 0;
        off = (size_t)p[0] | (size_t)p[1] << 8;
        p += 2;
        if (mlen == 15 && !get_len(&p, end, &mlen))
            return 0;
        mlen += MIN_MATCH;
        if (off == 0 || off > (size_t)(o - dst) ||
            (size_t)(oend - o) < mlen)
            return 0;
        while (mlen-- > 0) {        /* byte by byte: matches may overlap */
            *o = *(o - off);
            o++;
        }
    }
    return (size_t)(o - dst);
}
