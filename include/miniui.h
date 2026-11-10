/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MINIUI_H
#define MINIUI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mui_display mui_display;

typedef enum {
    MUI_AUTO,
    MUI_DRM,
    MUI_FBDEV,
} mui_backend;

typedef struct {
    mui_backend backend;
    const char *device; /* NULL probes standard device paths. */
} mui_options;

typedef struct {
    uint32_t width;
    uint32_t height;
    size_t stride;    /* Bytes per row. */
    uint32_t *pixels; /* Persistent native-endian 0x00RRGGBB pixels. */
} mui_surface;

/* NULL options selects automatic discovery. Errors return -1 and set errno.
 * All calls on a display must be serialized by the caller. */
int mui_open(mui_display **out, const mui_options *options);
const mui_surface *mui_buffer(mui_display *display);
mui_backend mui_backend_used(const mui_display *display);
int mui_present(mui_display *display);

/* Restores the prior display state where supported and releases all resources. */
int mui_close(mui_display *display);

#ifdef __cplusplus
}
#endif

#endif /* MINIUI_H */
