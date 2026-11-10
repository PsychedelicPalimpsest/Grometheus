/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "internal.h"
int mui_ioctl(int fd, unsigned long request, void *arg) {
    int rc;
    unsigned retries = 0;
    do {
        rc = ioctl(fd, request, arg);
    } while (rc < 0 && errno == EINTR && ++retries < 16);
    return rc;
}

int mui_close(mui_display *d) {
    if (!d) {
        return 0;
    }
    int rc = 0, err = 0;
    if (d->backend == MUI_DRM) {
        rc = mui_drm_close(d);
        err = errno;
    }
    if (d->backend == MUI_FBDEV) {
        mui_fb_restore(d);
    }
    free(d->saved_pixels);
    if (d->map) {
        munmap(d->map, d->map_size);
    }
    if (d->fd >= 0) {
        close(d->fd);
    }
    free(d->surface.pixels);
    free(d);
    if (rc) {
        errno = err;
    }
    return rc;
}

static int attempt(mui_display **out, mui_backend backend, const char *path) {
    mui_display *d = calloc(1, sizeof(*d));
    if (!d) {
        return -1;
    }
    d->fd = -1;
    d->backend = backend;
    int rc = backend == MUI_DRM ? mui_drm_open(d, path) : mui_fb_open(d, path);
    if (!rc) {
        uint64_t bytes = (uint64_t)d->surface.width * d->surface.height * 4;
        if (!bytes || bytes > MUI_MAX_BYTES) {
            errno = EOVERFLOW;
            rc = -1;
        } else {
            d->surface.stride = (size_t)d->surface.width * 4;
            d->surface.pixels = calloc(1, (size_t)bytes);
            if (!d->surface.pixels) {
                rc = -1;
            }
        }
    }
    if (rc) {
        int err = errno;
        mui_close(d);
        errno = err;
        return -1;
    }
    *out = d;
    return 0;
}

int mui_open(mui_display **out, const mui_options *options) {
    if (!out) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;
    mui_backend b = options ? options->backend : MUI_AUTO;
    const char *path = options ? options->device : NULL;
    if (b < MUI_AUTO || b > MUI_FBDEV || (path && b == MUI_AUTO)) {
        errno = EINVAL;
        return -1;
    }
    if (path) {
        return attempt(out, b, path);
    }
    int err = ENODEV;
    char name[64];
    if (b != MUI_FBDEV) {
        for (unsigned i = 0; i < 64; ++i) {
            snprintf(name, sizeof(name), "/dev/dri/card%u", i);
            if (!attempt(out, MUI_DRM, name)) {
                return 0;
            }
            if (errno != ENOENT && errno != ENODEV) {
                err = errno;
            }
        }
    }
    if (b != MUI_DRM) {
        for (unsigned i = 0; i < 32; ++i) {
            const char *formats[] = {"/dev/graphics/fb%u", "/dev/fb%u"};
            for (unsigned j = 0; j < 2; ++j) {
                snprintf(name, sizeof(name), formats[j], i);
                if (!attempt(out, MUI_FBDEV, name)) {
                    return 0;
                }
                if (errno != ENOENT && errno != ENODEV) {
                    err = errno;
                }
            }
        }
    }
    errno = err;
    return -1;
}

const mui_surface *mui_buffer(mui_display *d) {
    return d ? &d->surface : NULL;
}

mui_backend mui_backend_used(const mui_display *d) {
    return d ? d->backend : MUI_AUTO;
}

int mui_present(mui_display *d) {
    if (!d) {
        errno = EINVAL;
        return -1;
    }
    return d->backend == MUI_DRM ? mui_drm_present(d) : mui_fb_present(d);
}
