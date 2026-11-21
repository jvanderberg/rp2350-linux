#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(1);
    }

    return (double)ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

int main(void)
{
    const unsigned int batch = 10000;
    unsigned long long loops = 0;
    double start, elapsed;
    volatile unsigned int remaining;

    puts("Running delay-loop benchmark for 3 seconds...");
    fflush(stdout);

    start = now();

    do {
        remaining = batch;
        while (remaining != 0)
            remaining--;

        loops += batch;
        elapsed = now() - start;
    } while (elapsed < 3.0);

    printf("Loops:          %llu\n", loops);
    printf("Elapsed:        %.3f seconds\n", elapsed);
    printf("Loops/second:   %.0f\n", (double)loops / elapsed);
    printf("BogoMIPS-style: %.2f\n",
           (double)loops / elapsed / 500000.0);

    return 0;
}
