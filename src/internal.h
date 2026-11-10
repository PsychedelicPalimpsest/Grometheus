/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MUI_INTERNAL_H
#define MUI_INTERNAL_H

#include <linux/fb.h>

#include "drm.h"
#include "miniui.h"

#define MUI_MAX_DIM 16384u
#define MUI_MAX_BYTES (256u * 1024u * 1024u)
#define MUI_DRM_RESOURCE_LIMIT 128

struct mui_dumb {
    uint32_t handle;
    uint32_t fb;
    uint32_t pitch;
    size_t size;
    void *map;
};

struct mui_display {
    mui_surface surface;
    mui_backend backend;
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
    uint32_t saved_connectors[MUI_DRM_RESOURCE_LIMIT];
    uint32_t saved_count;
    struct mui_dumb dumb[2];
};

int mui_ioctl(int fd, unsigned long request, void *arg);

int mui_fb_open(mui_display *display, const char *path);
int mui_fb_present(mui_display *display);
void mui_fb_restore(mui_display *display);
int mui_fb_validate(const struct fb_fix_screeninfo *fix, const struct fb_var_screeninfo *var);
void mui_fb_copy(const mui_surface *surface, void *map, const struct fb_fix_screeninfo *fix,
                 const struct fb_var_screeninfo *var);

int mui_drm_open(mui_display *display, const char *path);
int mui_drm_present(mui_display *display);
int mui_drm_close(mui_display *display);

#endif /* MUI_INTERNAL_H */
