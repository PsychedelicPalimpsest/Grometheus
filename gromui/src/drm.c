/* SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2015 The Android Open Source Project
 * Adapted from TWRP minui/graphics_drm.cpp: connector selection, dumb buffers,
 * modesetting. Uses Linux UAPI directly in place of libdrm and android-base. */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "internal.h"

#define PTR(x) ((uint64_t)(uintptr_t)(x))

static int is_builtin_panel(uint32_t type) {
    return type == DRM_MODE_CONNECTOR_LVDS || type == DRM_MODE_CONNECTOR_eDP ||
           type == DRM_MODE_CONNECTOR_DSI;
}

static uint32_t find_crtc(int fd, const struct drm_mode_get_connector *connector,
                          const uint32_t *encoders, const struct drm_mode_card_res *resources,
                          const uint32_t *crtcs) {
    uint32_t fallback = 0;
    for (uint32_t i = 0; i < connector->count_encoders; ++i) {
        struct drm_mode_get_encoder encoder = {.encoder_id = encoders[i]};
        if (gromui_ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &encoder) < 0) {
            continue;
        }

        for (uint32_t j = 0; j < resources->count_crtcs && j < 32; ++j) {
            if (encoder.possible_crtcs & (1u << j)) {
                if (crtcs[j] == encoder.crtc_id) {
                    return crtcs[j];
                }
                if (!fallback) {
                    fallback = crtcs[j];
                }
            }
        }
    }
    return fallback;
}

static const struct drm_mode_modeinfo *preferred_mode(
    const struct drm_mode_get_connector *connector, const struct drm_mode_modeinfo *modes) {
    for (uint32_t i = 0; i < connector->count_modes; ++i) {
        if (modes[i].type & DRM_MODE_TYPE_PREFERRED) {
            return &modes[i];
        }
    }
    return &modes[0];
}

static int create_buffer(gromui_display *d, struct gromui_dumb *b) {
    struct drm_mode_create_dumb c = {
        .width = d->surface.width, .height = d->surface.height, .bpp = 32};
    if (gromui_ioctl(d->fd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) {
        return -1;
    }
    b->handle = c.handle;
    if (c.pitch < (uint64_t)c.width * 4 || c.size < (uint64_t)c.pitch * c.height ||
        c.size > GROMUI_MAX_BYTES) {
        errno = EOVERFLOW;
        return -1;
    }
    b->pitch = c.pitch;
    b->size = (size_t)c.size;
    struct drm_mode_fb_cmd fb = {.width = c.width,
                                 .height = c.height,
                                 .pitch = c.pitch,
                                 .bpp = 32,
                                 .depth = 24,
                                 .handle = c.handle};
    if (gromui_ioctl(d->fd, DRM_IOCTL_MODE_ADDFB, &fb)) {
        return -1;
    }
    b->fb = fb.fb_id;
    struct drm_mode_map_dumb m = {.handle = c.handle};
    if (gromui_ioctl(d->fd, DRM_IOCTL_MODE_MAP_DUMB, &m)) {
        return -1;
    }
    if (m.offset > INT64_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    b->map = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, (off_t)m.offset);
    if (b->map == MAP_FAILED) {
        b->map = NULL;
        return -1;
    }
    memset(b->map, 0, b->size);
    return 0;
}

static int get_connector(int fd, uint32_t id, struct drm_mode_get_connector *c, uint32_t *encoders,
                         struct drm_mode_modeinfo *modes) {
    memset(c, 0, sizeof(*c));
    c->connector_id = id;
    /* A zero-mode query forces detection on a freshly booted KMS device. */
    if (gromui_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, c)) {
        return -1;
    }
    c->count_props = 0;
    c->count_encoders = GROMUI_DRM_RESOURCE_LIMIT;
    c->encoders_ptr = PTR(encoders);
    c->count_modes = GROMUI_DRM_RESOURCE_LIMIT;
    c->modes_ptr = PTR(modes);
    if (gromui_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, c)) {
        return -1;
    }
    if (c->count_encoders > GROMUI_DRM_RESOURCE_LIMIT || c->count_modes > GROMUI_DRM_RESOURCE_LIMIT) {
        errno = EOVERFLOW;
        return -1;
    }
    return 0;
}

static int choose_display(gromui_display *d, const struct drm_mode_card_res *resources,
                          const uint32_t *connectors, const uint32_t *crtcs) {
    /* Search embedded panels first, then other connected outputs. */
    for (int panel_pass = 1; panel_pass >= 0; --panel_pass) {
        for (uint32_t i = 0; i < resources->count_connectors; ++i) {
            struct drm_mode_get_connector c;
            uint32_t encoders[GROMUI_DRM_RESOURCE_LIMIT];
            struct drm_mode_modeinfo modes[GROMUI_DRM_RESOURCE_LIMIT];
            if (get_connector(d->fd, connectors[i], &c, encoders, modes) || c.connection != 1 ||
                !c.count_modes) {
                continue;
            }
            if (is_builtin_panel(c.connector_type) != panel_pass) {
                continue;
            }

            uint32_t crtc = find_crtc(d->fd, &c, encoders, resources, crtcs);
            if (!crtc) {
                continue;
            }

            const struct drm_mode_modeinfo *mode = preferred_mode(&c, modes);
            if (!mode->hdisplay || !mode->vdisplay || mode->hdisplay > GROMUI_MAX_DIM ||
                mode->vdisplay > GROMUI_MAX_DIM) {
                continue;
            }

            d->saved.crtc_id = crtc;
            if (gromui_ioctl(d->fd, DRM_IOCTL_MODE_GETCRTC, &d->saved)) {
                continue;
            }
            d->connector = c.connector_id;
            d->crtc.crtc_id = crtc;
            d->crtc.mode = *mode;
            d->crtc.mode_valid = 1;
            d->crtc.set_connectors_ptr = PTR(&d->connector);
            d->crtc.count_connectors = 1;
            d->surface.width = mode->hdisplay;
            d->surface.height = mode->vdisplay;
            return 0;
        }
    }
    errno = ENODEV;
    return -1;
}

static int save_crtc_connectors(gromui_display *d, const struct drm_mode_card_res *resources,
                                const uint32_t *connectors) {
    for (uint32_t i = 0; i < resources->count_connectors; ++i) {
        struct drm_mode_get_connector connector;
        uint32_t encoders[GROMUI_DRM_RESOURCE_LIMIT];
        struct drm_mode_modeinfo modes[GROMUI_DRM_RESOURCE_LIMIT];
        if (get_connector(d->fd, connectors[i], &connector, encoders, modes) < 0) {
            return -1;
        }
        if (!connector.encoder_id) {
            continue;
        }

        struct drm_mode_get_encoder encoder = {.encoder_id = connector.encoder_id};
        if (gromui_ioctl(d->fd, DRM_IOCTL_MODE_GETENCODER, &encoder) < 0) {
            return -1;
        }
        if (encoder.crtc_id == d->saved.crtc_id) {
            d->saved_connectors[d->saved_count++] = connector.connector_id;
        }
    }
    return 0;
}

int gromui_drm_open(gromui_display *d, const char *path) {
    d->fd = open(path, O_RDWR | O_CLOEXEC);
    if (d->fd < 0) {
        return -1;
    }
    struct drm_get_cap cap = {.capability = DRM_CAP_DUMB_BUFFER};
    if (gromui_ioctl(d->fd, DRM_IOCTL_GET_CAP, &cap)) {
        return -1;
    }
    if (!cap.value) {
        errno = ENOTSUP;
        return -1;
    }
    /* Opening an unowned card normally acquires master; SET_MASTER also handles relinquished fds.
     */
    if (gromui_ioctl(d->fd, DRM_IOCTL_SET_MASTER, NULL)) {
        return -1;
    }
    uint32_t connectors[GROMUI_DRM_RESOURCE_LIMIT], crtcs[GROMUI_DRM_RESOURCE_LIMIT];
    struct drm_mode_card_res r = {.connector_id_ptr = PTR(connectors),
                                  .count_connectors = GROMUI_DRM_RESOURCE_LIMIT,
                                  .crtc_id_ptr = PTR(crtcs),
                                  .count_crtcs = GROMUI_DRM_RESOURCE_LIMIT};
    if (gromui_ioctl(d->fd, DRM_IOCTL_MODE_GETRESOURCES, &r)) {
        return -1;
    }
    if (r.count_connectors > GROMUI_DRM_RESOURCE_LIMIT || r.count_crtcs > GROMUI_DRM_RESOURCE_LIMIT) {
        errno = EOVERFLOW;
        return -1;
    }
    if (choose_display(d, &r, connectors, crtcs)) {
        return -1;
    }
    if (save_crtc_connectors(d, &r, connectors)) {
        return -1;
    }
    if (create_buffer(d, &d->dumb[0]) || create_buffer(d, &d->dumb[1])) {
        return -1;
    }
    d->crtc.fb_id = d->dumb[0].fb;
    if (gromui_ioctl(d->fd, DRM_IOCTL_MODE_SETCRTC, &d->crtc)) {
        return -1;
    }
    d->active = 1;
    d->front = 0;
    return 0;
}

int gromui_drm_present(gromui_display *d) {
    int next = 1 - d->front;
    struct gromui_dumb *b = &d->dumb[next];
    for (uint32_t y = 0; y < d->surface.height; ++y) {
        memcpy((unsigned char *)b->map + (size_t)y * b->pitch,
               (unsigned char *)d->surface.pixels + y * d->surface.stride, d->surface.stride);
    }
    /* Synchronous legacy KMS avoids dangling page-flip callbacks and unbounded event waits.
     * The old scanout is reused only after a successful modeset. */
    struct drm_mode_fb_dirty_cmd dirty = {.fb_id = b->fb};
    if (gromui_ioctl(d->fd, DRM_IOCTL_MODE_DIRTYFB, &dirty) && errno != ENOSYS && errno != EINVAL &&
        errno != ENOTTY) {
        return -1;
    }
    d->crtc.fb_id = b->fb;
    if (gromui_ioctl(d->fd, DRM_IOCTL_MODE_SETCRTC, &d->crtc)) {
        return -1;
    }
    d->front = next;
    return 0;
}

int gromui_drm_close(gromui_display *d) {
    int rc = 0, err = 0;
    if (d->fd < 0) {
        return 0;
    }
    if (d->active) {
        d->saved.set_connectors_ptr = PTR(d->saved_connectors);
        d->saved.count_connectors = d->saved_count;
        if (gromui_ioctl(d->fd, DRM_IOCTL_MODE_SETCRTC, &d->saved)) {
            rc = -1;
            err = errno;
        }
    }
    for (int i = 0; i < 2; ++i) {
        struct gromui_dumb *b = &d->dumb[i];
        if (b->map) {
            munmap(b->map, b->size);
        }
        if (b->fb) {
            gromui_ioctl(d->fd, DRM_IOCTL_MODE_RMFB, &b->fb);
        }
        if (b->handle) {
            struct drm_mode_destroy_dumb destroy = {.handle = b->handle};
            gromui_ioctl(d->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        }
    }
    if (rc) {
        errno = err;
    }
    return rc;
}
