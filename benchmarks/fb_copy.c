/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>

#include "../examples/timing.h"
#include "internal.h"

/* CPU/RAM microbenchmark. Does not model framebuffer memory or scanout latency. */
int main(void) {
    const unsigned sizes[][2] = {{800, 600}, {1920, 1080}};
    for (size_t i = 0; i < 2; ++i) {
        unsigned w = sizes[i][0], h = sizes[i][1];
        mui_surface s = {w, h, w * 4, malloc((size_t)w * h * 4)};
        void *dst = malloc((size_t)w * h * 4);
        if (!s.pixels || !dst) {
            return 1;
        }
        memset(s.pixels, 0x71, (size_t)w * h * 4);
        struct fb_fix_screeninfo f = {.line_length = w * 4};
        struct fb_var_screeninfo v = {
            .bits_per_pixel = 32, .red = {16, 8, 0}, .green = {8, 8, 0}, .blue = {0, 8, 0}};
        for (unsigned alpha = 0; alpha < 2; ++alpha) {
            v.transp = (struct fb_bitfield){24, alpha ? 8 : 0, 0};
            struct frame_timings t = {0};
            for (unsigned n = 0; n < 210; ++n) {
                uint64_t start = monotonic_ns();
                mui_fb_copy(&s, dst, &f, &v);
                if (n >= 10) {
                    record_frame(&t, monotonic_ns() - start);
                }
            }
            printf("%ux%u %s ", w, h, alpha ? "ARGB8888" : "XRGB8888");
            report_frames("copy", &t);
        }
        free(dst);
        free(s.pixels);
    }
    return 0;
}
