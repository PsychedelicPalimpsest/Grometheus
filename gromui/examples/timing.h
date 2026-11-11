/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GROMUI_EXAMPLE_TIMING_H
#define GROMUI_EXAMPLE_TIMING_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Example-only instrumentation: no timing or logging in the library hot path. */
struct frame_timings {
    uint64_t samples[256];
    size_t count;
};
static uint64_t monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000 + (uint64_t)now.tv_nsec;
}
static void record_frame(struct frame_timings *t, uint64_t elapsed) {
    if (t->count < sizeof(t->samples) / sizeof(t->samples[0])) {
        t->samples[t->count++] = elapsed;
    }
}
static int compare_times(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
static void report_frames(const char *name, struct frame_timings *t) {
    if (!t->count) {
        return;
    }
    qsort(t->samples, t->count, sizeof(t->samples[0]), compare_times);
    printf("timing %s n=%zu median_us=%.1f p95_us=%.1f max_us=%.1f\n", name, t->count,
           t->samples[t->count / 2] / 1000.0, t->samples[(t->count - 1) * 95 / 100] / 1000.0,
           t->samples[t->count - 1] / 1000.0);
}
#endif
