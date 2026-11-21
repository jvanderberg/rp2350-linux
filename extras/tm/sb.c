/*
 * sb.c - compressed scrollback. See sb.h.
 *
 * Encoded line format: UTF-8 text; the byte 0x01 followed by a 4-byte
 * little-endian attribute word changes the attribute of the characters
 * after it (lines start with the default attribute); '\n' ends the line.
 * Control characters never appear in cells, so 0x01 is unambiguous.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lz.h"
#include "sb.h"
#include "util.h"

#define MIN_RING (16 * SB_BLOCK)

void sb_init(struct sb *sb, const char *dir, size_t disk_bytes,
             size_t ram_bytes)
{
    static int counter;

    memset(sb, 0, sizeof *sb);
    sb->fd = -1;
    sb->cached = -1;
    sb->budget = ram_bytes;
    if (dir != NULL && *dir != '\0') {
        char path[512];

        mkdir(dir, 0700);
        snprintf(path, sizeof path, "%s/tm-%ld-%d.hist", dir,
                 (long)getpid(), counter++);
        sb->fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (sb->fd >= 0) {
            unlink(path);       /* nameless: vanishes when closed */
            sb->ring_size = (long)(disk_bytes > MIN_RING ? disk_bytes
                                                         : MIN_RING);
        }
    }
}

static void drop_first(struct sb *sb)
{
    struct sb_block *b = &sb->blk[sb->first];

    if (b->mem != NULL) {
        sb->used -= b->len;
        free(b->mem);
    }
    sb->blines -= b->lines;
    sb->dropped += b->lines;
    if (sb->cached == sb->first)
        sb->cached = -1;
    sb->first++;
    sb->nblk--;
}

void sb_clear(struct sb *sb)
{
    while (sb->nblk > 0)
        drop_first(sb);
    sb->dropped += sb->open_lines;
    sb->open_len = 0;
    sb->open_lines = 0;
    sb->wpos = 0;
}

void sb_free(struct sb *sb)
{
    sb_clear(sb);
    free(sb->blk);
    free(sb->open);
    free(sb->cache);
    if (sb->fd >= 0)
        close(sb->fd);
    memset(sb, 0, sizeof *sb);
    sb->fd = -1;
    sb->cached = -1;
}

/* Make room for one more index entry at the end. */
static struct sb_block *new_entry(struct sb *sb)
{
    if (sb->first > 0 && sb->first + sb->nblk == sb->cap) {
        memmove(sb->blk, sb->blk + sb->first, sizeof *sb->blk * (size_t)sb->nblk);
        sb->first = 0;
        sb->cached = -1;        /* indices moved */
    }
    if (sb->nblk == sb->cap) {
        sb->cap = sb->cap ? sb->cap * 2 : 32;
        sb->blk = xrealloc(sb->blk, sizeof *sb->blk * (size_t)sb->cap);
    }
    return &sb->blk[sb->first + sb->nblk++];
}

/* Write a finished block to the ring file. Returns its offset or -1. */
static long ring_write(struct sb *sb, const unsigned char *data, size_t len)
{
    long pos = sb->wpos, last = -1;
    int k;

    if (pos + (long)len > sb->ring_size)
        pos = 0;
    /* Everything as old as the newest block we overwrite must go, so the
     * history stays one unbroken run of lines. */
    for (k = 0; k < sb->nblk; k++) {
        const struct sb_block *b = &sb->blk[sb->first + k];

        if (b->off >= 0 && b->off < pos + (long)len && pos < b->off + b->len)
            last = k;
    }
    while (last-- >= 0)
        drop_first(sb);
    if (pwrite(sb->fd, data, len, pos) != (ssize_t)len)
        return -1;
    sb->wpos = pos + (long)len;
    return pos;
}

/* Compress the open block and file it. */
static void flush(struct sb *sb)
{
    unsigned char *c = xmalloc(sb->open_len);
    size_t clen = lz_compress(sb->open, sb->open_len, c, sb->open_len);
    const unsigned char *data = clen ? c : sb->open;
    size_t len = clen ? clen : sb->open_len;
    struct sb_block blk;

    blk.packed = clen != 0;
    blk.len = (unsigned short)len;
    blk.raw_len = (unsigned short)sb->open_len;
    blk.lines = (unsigned short)sb->open_lines;
    blk.mem = NULL;
    blk.off = sb->fd >= 0 ? ring_write(sb, data, len) : -1;
    if (blk.off < 0) {                  /* RAM mode, or the write failed */
        blk.mem = xmalloc(len);
        memcpy(blk.mem, data, len);
        sb->used += len;
    }
    *new_entry(sb) = blk;
    sb->blines += sb->open_lines;
    sb->open_len = 0;
    sb->open_lines = 0;
    while (sb->used > sb->budget && sb->nblk > 1)
        drop_first(sb);
    free(c);
}

void sb_push(struct sb *sb, const struct cell *c, int n)
{
    uint32_t cur = ATTR_DEFAULT;
    int last = n - 1, i;
    size_t need;
    unsigned char *o;

    while (last >= 0 && (c[last].ch == ' ' || c[last].ch == 0) &&
           (c[last].attr & ~CELL_CONT) == ATTR_DEFAULT)
        last--;
    need = sb->open_len + (size_t)(last + 1) * 9 + 2;
    if (need > LZ_MAX_BLOCK)
        return;                         /* absurdly wide line: skip */
    if (sb->open == NULL || need > (size_t)SB_BLOCK * 3) {
        /* the buffer is sized for SB_BLOCK plus one long line */
        size_t cap = need > (size_t)SB_BLOCK * 3 ? need : (size_t)SB_BLOCK * 3;

        sb->open = xrealloc(sb->open, cap);
    }
    o = sb->open + sb->open_len;
    for (i = 0; i <= last; i++) {
        uint32_t a = c[i].attr & ~CELL_CONT, ch = c[i].ch;

        if (c[i].attr & CELL_CONT)
            continue;
        if (a != cur) {
            *o++ = 1;
            *o++ = (unsigned char)a;
            *o++ = (unsigned char)(a >> 8);
            *o++ = (unsigned char)(a >> 16);
            *o++ = (unsigned char)(a >> 24);
            cur = a;
        }
        if (ch < 0x20 || ch == 0x7F)
            ch = ' ';
        o += utf8_encode((char *)o, ch);
    }
    *o++ = '\n';
    sb->open_len = (size_t)(o - sb->open);
    sb->open_lines++;
    if (sb->open_len >= SB_BLOCK)
        flush(sb);
}

long sb_count(const struct sb *sb)
{
    return sb->blines + sb->open_lines;
}

int sb_on_disk(const struct sb *sb)
{
    return sb->fd >= 0;
}

/* Bring block k (array index) into the cache, decompressed. */
static int load(struct sb *sb, int k)
{
    struct sb_block *b = &sb->blk[k];
    unsigned char *tmp;
    const unsigned char *data;

    if (sb->cached == k)
        return 1;
    if (sb->cache_len < b->raw_len) {
        sb->cache = xrealloc(sb->cache, b->raw_len);
        sb->cache_len = b->raw_len;
    }
    tmp = NULL;
    if (b->mem != NULL) {
        data = b->mem;
    } else {
        tmp = xmalloc(b->len);
        if (pread(sb->fd, tmp, b->len, b->off) != (ssize_t)b->len) {
            free(tmp);
            return 0;
        }
        data = tmp;
    }
    if (b->packed) {
        if (lz_decompress(data, b->len, sb->cache, b->raw_len) != b->raw_len) {
            free(tmp);
            return 0;
        }
    } else {
        memcpy(sb->cache, data, b->len);
    }
    free(tmp);
    sb->cached = k;
    return 1;
}

static void decode(const unsigned char *p, const unsigned char *end,
                   struct cell *out, int cols)
{
    uint32_t attr = ATTR_DEFAULT;
    int x = 0, i;

    for (i = 0; i < cols; i++) {
        out[i].ch = ' ';
        out[i].attr = ATTR_DEFAULT;
    }
    while (p < end && *p != '\n') {
        uint32_t cp;
        int used, w;

        if (*p == 1 && end - p >= 5) {
            attr = (uint32_t)p[1] | (uint32_t)p[2] << 8 |
                   (uint32_t)p[3] << 16 | (uint32_t)p[4] << 24;
            p += 5;
            continue;
        }
        cp = utf8_decode(p, (size_t)(end - p), &used);
        if (used == 0)
            break;
        p += used;
        w = char_width(cp);
        if (w == 0)
            continue;               /* combining marks are not kept */
        if (x + w > cols)
            break;
        out[x].ch = cp;
        out[x].attr = attr;
        if (w == 2) {
            out[x + 1].ch = ' ';
            out[x + 1].attr = attr | CELL_CONT;
        }
        x += w;
    }
}

int sb_get(struct sb *sb, long i, struct cell *out, int cols)
{
    const unsigned char *p, *end;
    int k;

    if (i < 0 || i >= sb_count(sb))
        return 0;
    if (i >= sb->blines) {
        p = sb->open;
        end = sb->open + sb->open_len;
        i -= sb->blines;
    } else {
        for (k = sb->first; i >= sb->blk[k].lines; k++)
            i -= sb->blk[k].lines;
        if (!load(sb, k))
            return 0;
        p = sb->cache;
        end = sb->cache + sb->blk[k].raw_len;
    }
    while (i > 0 && p < end) {          /* skip to the line */
        const unsigned char *nl = memchr(p, '\n', (size_t)(end - p));

        if (nl == NULL)
            return 0;
        p = nl + 1;
        i--;
    }
    decode(p, end, out, cols);
    return 1;
}
