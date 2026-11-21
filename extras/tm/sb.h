/*
 * sb.h - compressed scrollback history for one window.
 *
 * Lines that scroll off the top of the screen are stored in two stages:
 *
 *  1. Encoded compactly: UTF-8 text, a 5-byte attribute change only where
 *     the colour/style changes, trailing blanks dropped, '\n' at the end.
 *     A typical shell line shrinks from cols * 8 bytes of cells to its
 *     text length.
 *  2. Collected into blocks of about SB_BLOCK bytes, each compressed with
 *     lz.c when full.
 *
 * Finished blocks go either to a ring file on disk (only an index entry
 * stays in RAM) or, if no directory is set or it can't be used, to RAM.
 * Either way there is a byte budget: when it is exceeded the oldest
 * blocks are dropped. The file is unlinked as soon as it is created, so
 * it disappears by itself when the window or the server goes away.
 *
 * Lines are numbered from 0 (oldest kept) to sb_count() - 1 (newest).
 * Dropping old blocks renumbers everything; sb->dropped counts the lines
 * dropped so far, so a viewer can keep its place.
 */
#ifndef SB_H
#define SB_H

#include <stddef.h>

#include "cell.h"

#define SB_BLOCK 4096

struct sb_block {
    long off;               /* file offset, or -1 when held in RAM */
    unsigned char *mem;     /* RAM copy (RAM mode only) */
    unsigned short len;     /* stored bytes */
    unsigned short raw_len; /* bytes after decompression */
    unsigned short lines;
    unsigned char packed;   /* compressed (else stored raw) */
};

struct sb {
    struct sb_block *blk;   /* blocks, oldest first, from blk[first] */
    int first, nblk, cap;
    long blines;            /* lines in the finished blocks */
    long dropped;           /* lines dropped since the start */

    unsigned char *open;    /* the block being filled (uncompressed) */
    size_t open_len;
    int open_lines;

    int fd;                 /* ring file, or -1 for RAM mode */
    long ring_size, wpos;   /* ring capacity and next write offset */
    size_t budget, used;    /* RAM mode: byte budget and bytes held */

    int cached;             /* block index held in cache, -1 none */
    unsigned char *cache;
    size_t cache_len;
};

/* Set up an empty history: in a ring file of `disk_bytes` in `dir`, or,
 * if dir is NULL or unusable, in `ram_bytes` of memory. */
void sb_init(struct sb *sb, const char *dir, size_t disk_bytes,
             size_t ram_bytes);
void sb_free(struct sb *sb);
void sb_clear(struct sb *sb);

/* Append a line of n cells. */
void sb_push(struct sb *sb, const struct cell *cells, int n);

/* Number of lines held. */
long sb_count(const struct sb *sb);

/* Decode line i into out (cols cells, padded with blanks). Returns 0 if
 * it can't be read. */
int sb_get(struct sb *sb, long i, struct cell *out, int cols);

/* Is the history on disk? */
int sb_on_disk(const struct sb *sb);

#endif /* SB_H */
