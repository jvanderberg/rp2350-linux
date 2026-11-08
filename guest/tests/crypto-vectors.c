/* Runs a vector file (rvemu-rs/scripts/board-crypto-vectors.py) through
 * /dev/rvemu-crypto. Every case runs REPEATS times on one descriptor, so
 * the firmware's workspace is reused across operations, sizes and failures.
 * An error must be the expected errno and leave the output buffer as it
 * was (the kernel never copies output back after a failure).
 *
 *   crypto-vectors FILE [REPEATS]          check every case
 *   crypto-vectors FILE --bench N [NAME]   per case (or the one NAME): a
 *       benchmark point, N calls, a point, then the firmware's report on the
 *       console (cycles between the two points) */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "rvemu-crypto.h"
#include "rvemu-ssh.h"

struct vec {
    char name[48];
    uint32_t op, err, cap, src_len, params_len, expected_len;
    const unsigned char *src, *params, *expected;
};

static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static int run(int fd, const struct vec *v, unsigned char *out, uint32_t *len)
{
    struct rvemu_crypto_request r = {
        .operation = v->op, .source = (uintptr_t)v->src, .source_len = v->src_len,
        .destination = (uintptr_t)out, .capacity = v->cap,
        .parameters = (uintptr_t)v->params, .parameters_len = v->params_len,
    };
    if (ioctl(fd, RVEMU_CRYPTO_RUN, &r)) return errno;
    *len = r.output_len;
    return 0;
}

static int check(int fd, const struct vec *v, unsigned char *out)
{
    uint32_t len = 0;
    memset(out, 0xa5, RVEMU_CRYPTO_MAX_BUFFER);
    int e = run(fd, v, out, &len);
    if ((uint32_t)e != v->err) {
        printf("FAIL %s: errno %d, expected %u\n", v->name, e, v->err);
        return 1;
    }
    if (e) {
        for (uint32_t i = 0; i < v->cap; i++)
            if (out[i] != 0xa5) { printf("FAIL %s: output changed on error\n", v->name); return 1; }
        return 0;
    }
    if (len != v->expected_len || memcmp(out, v->expected, len)) {
        printf("FAIL %s: output (%u bytes) differs from the expected %u\n", v->name, len, v->expected_len);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: crypto-vectors FILE [REPEATS | --bench N [NAME]]\n"); return 2; }
    int bench = argc >= 4 && !strcmp(argv[2], "--bench");
    int reps = bench ? atoi(argv[3]) : argc >= 3 ? atoi(argv[2]) : 1;
    const char *only = bench && argc >= 5 ? argv[4] : NULL;
    int f = open(argv[1], O_RDONLY);
    struct stat st;
    if (f < 0 || fstat(f, &st)) { perror(argv[1]); return 2; }
    unsigned char *file = malloc(st.st_size), *out = malloc(RVEMU_CRYPTO_MAX_BUFFER);
    if (!file || !out || read(f, file, st.st_size) != st.st_size || memcmp(file, "RVCV", 4)) {
        fprintf(stderr, "%s: not a vector file\n", argv[1]);
        return 2;
    }
    close(f);
    uint32_t n = le32(file + 4);
    struct vec *v = calloc(n, sizeof(*v));
    const unsigned char *p = file + 8, *end = file + st.st_size;
    for (uint32_t i = 0; i < n; i++) {
        if (p + 72 > end) { fprintf(stderr, "truncated vector file\n"); return 2; }
        memcpy(v[i].name, p, 47);
        v[i].op = le32(p + 48); v[i].err = le32(p + 52); v[i].cap = le32(p + 56);
        v[i].src_len = le32(p + 60); v[i].params_len = le32(p + 64); v[i].expected_len = le32(p + 68);
        p += 72;
        v[i].src = p; p += v[i].src_len;
        v[i].params = p; p += v[i].params_len;
        v[i].expected = p; p += v[i].expected_len;
        if (p > end) { fprintf(stderr, "truncated vector file\n"); return 2; }
    }
    int fd = open("/dev/rvemu-crypto", O_RDWR);
    if (fd < 0) { perror("/dev/rvemu-crypto"); return 1; }
    if (bench) {
        int b = open("/dev/rvemu-bench", O_RDWR);
        if (b < 0) { perror("/dev/rvemu-bench"); return 1; }
        for (uint32_t i = 0; i < n; i++) {
            if (only ? strcmp(v[i].name, only) : v[i].err != 0) continue;
            if (check(fd, &v[i], out)) return 1;
            uint32_t len, t0 = 0, t1 = 1;
            printf("BENCH CASE %s op=%u calls=%d\n", v[i].name, v[i].op, reps);
            fflush(stdout);
            if (ioctl(b, RVEMU_BENCH_POINT, &t0)) { perror("benchmark point"); return 1; }
            for (int r = 0; r < reps; r++) run(fd, &v[i], out, &len);
            if (ioctl(b, RVEMU_BENCH_POINT, &t1)) { perror("benchmark point"); return 1; }
            if (ioctl(b, RVEMU_BENCH_REPORT) < 0) { perror("benchmark report"); return 1; }
        }
        puts("BENCH DONE");
        return 0;
    }
    int failed = 0;
    for (int r = 0; r < reps; r++)
        for (uint32_t i = 0; i < n; i++) failed += check(fd, &v[i], out);
    if (failed) { printf("CRYPTO VECTORS FAIL %d of %u x %d\n", failed, n, reps); return 1; }
    printf("CRYPTO VECTORS PASS %u cases x %d\n", n, reps);
    return 0;
}
