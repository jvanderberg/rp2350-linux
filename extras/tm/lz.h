/*
 * lz.h - a small LZ77 block compressor for the scrollback.
 *
 * The format is LZ4's block format (a token byte with literal and match
 * lengths, the literals, a 2-byte little-endian back-reference offset,
 * length extensions in runs of 255), so the decoder is a dozen lines and
 * very fast. The encoder is a greedy single-hash matcher: it gets most of
 * the benefit on terminal text, which is full of repeated prompts, paths
 * and log prefixes, and needs only an 8 KB table.
 *
 * Blocks are at most LZ_MAX_BLOCK bytes.
 */
#ifndef LZ_H
#define LZ_H

#include <stddef.h>

#define LZ_MAX_BLOCK 65535

/* Compress src[0, n) into dst (room for cap bytes). Returns the
 * compressed size, or 0 if the result would not be smaller than n (the
 * caller then stores the block uncompressed). */
size_t lz_compress(const unsigned char *src, size_t n, unsigned char *dst,
                   size_t cap);

/* Decompress src[0, n) into dst (room for cap bytes). Returns the size
 * produced, or 0 if the input is corrupt or does not fit. */
size_t lz_decompress(const unsigned char *src, size_t n, unsigned char *dst,
                     size_t cap);

#endif /* LZ_H */
