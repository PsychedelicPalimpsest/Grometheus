/* SPDX-License-Identifier: Apache-2.0
 * Derived from AOSP/TWRP minui/graphics_fbdev.cpp, Copyright 2014 AOSP.
 * Retains mapped framebuffer presentation; removes Android and drawing layers. */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "internal.h"
static int field_valid(struct fb_bitfield b, unsigned bits, int optional) {
    return (!b.length && optional) || (b.length && b.length <= 8 && b.offset < bits &&
                                       b.length <= bits - b.offset && !b.msb_right);
}

static uint32_t mask(struct fb_bitfield b) {
    return b.length ? ((1u << b.length) - 1) << b.offset : 0;
}

int gromui_fb_validate(const struct fb_fix_screeninfo *f, const struct fb_var_screeninfo *v) {
    const unsigned bits = v->bits_per_pixel;
    const unsigned bytes = bits / 8;

    const int supported_format = f->type == FB_TYPE_PACKED_PIXELS &&
                                 f->visual == FB_VISUAL_TRUECOLOR && !v->grayscale && !v->nonstd &&
                                 (bits == 16 || bits == 24 || bits == 32) &&
                                 field_valid(v->red, bits, 0) && field_valid(v->green, bits, 0) &&
                                 field_valid(v->blue, bits, 0) && field_valid(v->transp, bits, 1);
    if (!supported_format) {
        errno = ENOTSUP;
        return -1;
    }

    const uint32_t red = mask(v->red);
    const uint32_t green = mask(v->green);
    const uint32_t blue = mask(v->blue);
    const uint32_t alpha = mask(v->transp);
    const uint64_t right = (uint64_t)v->xoffset + v->xres;
    const uint64_t bottom = (uint64_t)v->yoffset + v->yres;

    const int nonoverlapping_channels =
        !(red & green) && !(red & blue) && !(green & blue) && !(alpha & (red | green | blue));
    const int valid_bounds =
        v->xres && v->yres && v->xres <= GROMUI_MAX_DIM && v->yres <= GROMUI_MAX_DIM &&
        right <= v->xres_virtual && bottom <= v->yres_virtual && right * bytes <= f->line_length &&
        bottom * f->line_length <= f->smem_len && f->smem_len && f->smem_len <= GROMUI_MAX_BYTES;

    if (!nonoverlapping_channels || !valid_bounds) {
        errno = ENOTSUP;
        return -1;
    }
    return 0;
}

int gromui_fb_open(gromui_display *d, const char *path) {
    d->fd = open(path, O_RDWR | O_CLOEXEC);
    if (d->fd < 0) {
        return -1;
    }
    if (gromui_ioctl(d->fd, FBIOGET_FSCREENINFO, &d->fix) ||
        gromui_ioctl(d->fd, FBIOGET_VSCREENINFO, &d->var) || gromui_fb_validate(&d->fix, &d->var)) {
        return -1;
    }
    d->map_size = d->fix.smem_len;
    d->map = mmap(NULL, d->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, 0);
    if (d->map == MAP_FAILED) {
        d->map = NULL;
        return -1;
    }
    d->surface.width = d->var.xres;
    d->surface.height = d->var.yres;
    size_t row = (size_t)d->var.xres * (d->var.bits_per_pixel / 8);
    if ((uint64_t)row * d->var.yres > GROMUI_MAX_BYTES) {
        errno = EOVERFLOW;
        return -1;
    }
    d->saved_pixels = malloc(row * d->var.yres);
    if (!d->saved_pixels) {
        return -1;
    }
    for (uint32_t y = 0; y < d->var.yres; ++y) {
        memcpy(d->saved_pixels + y * row,
               (unsigned char *)d->map + ((size_t)y + d->var.yoffset) * d->fix.line_length +
                   (size_t)d->var.xoffset * (d->var.bits_per_pixel / 8),
               row);
    }
    /* Unblank is best effort: many fbdev drivers don't implement it. */
    gromui_ioctl(d->fd, FBIOBLANK, (void *)(uintptr_t)FB_BLANK_UNBLANK);
    return 0;
}

static uint32_t channel(uint32_t c, struct fb_bitfield b) {
    return b.length ? ((c * ((1u << b.length) - 1) + 127) / 255) << b.offset : 0;
}

void gromui_fb_copy(const gromui_surface *s, void *map, const struct fb_fix_screeninfo *f,
                 const struct fb_var_screeninfo *v) {
    unsigned bytes = v->bits_per_pixel / 8;
    const uint32_t alpha = mask(v->transp);
    const int native_rgb = bytes == 4 && v->red.length == 8 && v->red.offset == 16 &&
                           v->green.length == 8 && v->green.offset == 8 && v->blue.length == 8 &&
                           v->blue.offset == 0;
    for (uint32_t y = 0; y < s->height; ++y) {
        const uint32_t *src = (const uint32_t *)((const unsigned char *)s->pixels + y * s->stride);
        unsigned char *dst = (unsigned char *)map + ((size_t)y + v->yoffset) * f->line_length +
                             (size_t)v->xoffset * bytes;
        if (native_rgb && !v->transp.length) {
            /* X is ignored by scanout; preserve the caller's unused high byte. */
            memcpy(dst, src, (size_t)s->width * 4);
            continue;
        }
        if (native_rgb) {
            for (uint32_t x = 0; x < s->width; ++x) {
                uint32_t p = (src[x] & 0x00ffffffu) | alpha;
                /* memcpy also supports drivers with an unaligned row pitch. */
                memcpy(dst + (size_t)x * 4, &p, sizeof(p));
            }
            continue;
        }
        for (uint32_t x = 0; x < s->width; ++x) {
            uint32_t p = channel((src[x] >> 16) & 255, v->red) |
                         channel((src[x] >> 8) & 255, v->green) | channel(src[x] & 255, v->blue) |
                         alpha;
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
            memcpy(dst + x * bytes, (unsigned char *)&p + 4 - bytes, bytes);
#else
            memcpy(dst + x * bytes, &p, bytes);
#endif
        }
    }
}

int gromui_fb_present(gromui_display *d) {
    gromui_fb_copy(&d->surface, d->map, &d->fix, &d->var);
    return 0;
}

void gromui_fb_restore(gromui_display *d) {
    if (!d->map || !d->saved_pixels) {
        return;
    }
    size_t row = (size_t)d->var.xres * (d->var.bits_per_pixel / 8);
    for (uint32_t y = 0; y < d->var.yres; ++y) {
        memcpy((unsigned char *)d->map + ((size_t)y + d->var.yoffset) * d->fix.line_length +
                   (size_t)d->var.xoffset * (d->var.bits_per_pixel / 8),
               d->saved_pixels + y * row, row);
    }
}
