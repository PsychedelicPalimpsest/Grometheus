/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GROMUI_H
#define GROMUI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gromui_display gromui_display;

typedef enum {
    GROMUI_AUTO,
    GROMUI_DRM,
    GROMUI_FBDEV,
} gromui_backend;

typedef struct {
    gromui_backend backend;
    const char *device; /* NULL probes standard device paths. */
} gromui_options;

typedef struct {
    uint32_t width;
    uint32_t height;
    size_t stride;    /* Bytes per row. */
    uint32_t *pixels; /* Persistent native-endian 0x00RRGGBB pixels. */
} gromui_surface;

/* NULL options selects automatic discovery. Errors return -1 and set errno.
 * All calls on a display must be serialized by the caller. */
int gromui_open(gromui_display **out, const gromui_options *options);
const gromui_surface *gromui_buffer(gromui_display *display);
gromui_backend gromui_backend_used(const gromui_display *display);
int gromui_present(gromui_display *display);

/* Restores the prior display state where supported and releases all resources. */
int gromui_close(gromui_display *display);

#ifdef __cplusplus
}
#endif

#endif /* GROMUI_H */
