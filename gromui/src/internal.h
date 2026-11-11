/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GROMUI_INTERNAL_H
#define GROMUI_INTERNAL_H

#include <linux/fb.h>

#include "drm.h"
#include "gromui.h"

#define GROMUI_MAX_DIM 16384u
#define GROMUI_MAX_BYTES (256u * 1024u * 1024u)
#define GROMUI_DRM_RESOURCE_LIMIT 128

struct gromui_dumb {
    uint32_t handle;
    uint32_t fb;
    uint32_t pitch;
    size_t size;
    void *map;
};

struct gromui_display {
    gromui_surface surface;
    gromui_backend backend;
    int fd;

    /* fbdev state */
    void *map;
    size_t map_size;
    unsigned char *saved_pixels;
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;

    /* DRM state */
    int active;
    int front;
    struct drm_mode_crtc saved;
    struct drm_mode_crtc crtc;
    uint32_t connector;
    uint32_t saved_connectors[GROMUI_DRM_RESOURCE_LIMIT];
    uint32_t saved_count;
    struct gromui_dumb dumb[2];
};

int gromui_ioctl(int fd, unsigned long request, void *arg);

int gromui_fb_open(gromui_display *display, const char *path);
int gromui_fb_present(gromui_display *display);
void gromui_fb_restore(gromui_display *display);
int gromui_fb_validate(const struct fb_fix_screeninfo *fix, const struct fb_var_screeninfo *var);
void gromui_fb_copy(const gromui_surface *surface, void *map, const struct fb_fix_screeninfo *fix,
                 const struct fb_var_screeninfo *var);

int gromui_drm_open(gromui_display *display, const char *path);
int gromui_drm_present(gromui_display *display);
int gromui_drm_close(gromui_display *display);

#endif /* GROMUI_INTERNAL_H */
