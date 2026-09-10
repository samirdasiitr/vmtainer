/*
 * Copyright (c) 2026 Samir Das <samiruor@gmail.com>. All rights reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL.
 * Unauthorized copying, reproduction, distribution, or modification of this
 * file, via any medium, is strictly prohibited.
 * All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv) {
    size_t mb = 200;
    if (argc > 1) {
        mb = (size_t)atoi(argv[1]);
        if (mb == 0) mb = 200;
    }

    size_t bytes = mb * 1024ULL * 1024ULL;
    size_t pages = bytes / 4096;

    printf("[MEM_BENCH] Allocating %zu MB (%zu pages) of new memory...\n", mb, pages);

    char *buf = (char *)malloc(bytes);
    if (!buf) {
        perror("malloc");
        return 1;
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    volatile char *p = (volatile char *)buf;
    for (size_t i = 0; i < bytes; i += 4096) {
        p[i] = (char)(i & 0xff);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);

    double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
                (t1.tv_nsec - t0.tv_nsec) / 1000000.0;
    double throughput_mb_s = (double)mb / (ms / 1000.0);
    double throughput_gb_s = throughput_mb_s / 1024.0;

    printf("[MEM_BENCH] Result: %zu MB in %.2f ms (throughput: %.1f MB/s / %.2f GB/s)\n",
           mb, ms, throughput_mb_s, throughput_gb_s);

    free(buf);
    return 0;
}
