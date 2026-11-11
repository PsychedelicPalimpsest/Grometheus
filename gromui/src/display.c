/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "internal.h"
int gromui_ioctl(int fd, unsigned long request, void *arg) {
    int rc;
    unsigned retries = 0;
    do {
        rc = ioctl(fd, request, arg);
    } while (rc < 0 && errno == EINTR && ++retries < 16);
    return rc;
}

int gromui_close(gromui_display *d) {
    if (!d) {
        return 0;
    }
    int rc = 0, err = 0;
    if (d->backend == GROMUI_DRM) {
        rc = gromui_drm_close(d);
        err = errno;
    }
    if (d->backend == GROMUI_FBDEV) {
        gromui_fb_restore(d);
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

static int attempt(gromui_display **out, gromui_backend backend, const char *path) {
    gromui_display *d = calloc(1, sizeof(*d));
    if (!d) {
        return -1;
    }
    d->fd = -1;
    d->backend = backend;
    int rc = backend == GROMUI_DRM ? gromui_drm_open(d, path) : gromui_fb_open(d, path);
    if (!rc) {
        uint64_t bytes = (uint64_t)d->surface.width * d->surface.height * 4;
        if (!bytes || bytes > GROMUI_MAX_BYTES) {
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
        gromui_close(d);
        errno = err;
        return -1;
    }
    *out = d;
    return 0;
}

/* Discover only present nodes. Scanning every possible pathname on each
 * preboot retry generated thousands of failed open calls on empty /dev. */
static uint64_t device_numbers(const char *directory, const char *prefix, unsigned limit) {
    DIR *dir = opendir(directory);
    if (!dir) {
        /* A searchable directory may still deny listing. Preserve the old
         * numeric probe in that unusual case. */
        if (errno == EACCES || errno == EPERM) {
            return UINT64_MAX;
        }
        return 0;
    }
    uint64_t found = 0;
    size_t length = strlen(prefix);
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(dir))) {
        const char *name = entry->d_name;
        if (memcmp(name, prefix, length) != 0) {
            continue;
        }
        const char *digits = name + length;
        if (*digits < '0' || *digits > '9') {
            continue;
        }
        unsigned number = 0;
        for (; *digits >= '0' && *digits <= '9'; ++digits) {
            number = number * 10 + (unsigned)(*digits - '0');
            if (number >= limit) {
                break;
            }
        }
        if (!*digits && number < limit) {
            found |= UINT64_C(1) << number;
        }
    }
    int read_error = errno;
    closedir(dir);
    return read_error ? UINT64_MAX : found;
}

int gromui_open(gromui_display **out, const gromui_options *options) {
    if (!out) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;
    gromui_backend b = options ? options->backend : GROMUI_AUTO;
    const char *path = options ? options->device : NULL;
    if (b < GROMUI_AUTO || b > GROMUI_FBDEV || (path && b == GROMUI_AUTO)) {
        errno = EINVAL;
        return -1;
    }
    if (path) {
        return attempt(out, b, path);
    }
    int err = ENODEV;
    char name[64];
    if (b != GROMUI_FBDEV) {
        uint64_t cards = device_numbers("/dev/dri", "card", 64);
        for (unsigned i = 0; i < 64; ++i) {
            if (!(cards & (UINT64_C(1) << i))) {
                continue;
            }
            snprintf(name, sizeof(name), "/dev/dri/card%u", i);
            if (!attempt(out, GROMUI_DRM, name)) {
                return 0;
            }
            if (errno != ENOENT && errno != ENODEV) {
                err = errno;
            }
        }
    }
    if (b != GROMUI_DRM) {
        uint64_t graphics = device_numbers("/dev/graphics", "fb", 32);
        uint64_t root = device_numbers("/dev", "fb", 32);
        for (unsigned i = 0; i < 32; ++i) {
            const char *formats[] = {"/dev/graphics/fb%u", "/dev/fb%u"};
            const uint64_t masks[] = {graphics, root};
            for (unsigned j = 0; j < 2; ++j) {
                if (!(masks[j] & (UINT64_C(1) << i))) {
                    continue;
                }
                snprintf(name, sizeof(name), formats[j], i);
                if (!attempt(out, GROMUI_FBDEV, name)) {
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

const gromui_surface *gromui_buffer(gromui_display *d) {
    return d ? &d->surface : NULL;
}

gromui_backend gromui_backend_used(const gromui_display *d) {
    return d ? d->backend : GROMUI_AUTO;
}

int gromui_present(gromui_display *d) {
    if (!d) {
        errno = EINVAL;
        return -1;
    }
    return d->backend == GROMUI_DRM ? gromui_drm_present(d) : gromui_fb_present(d);
}
