/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "miniui_lvgl.h"
struct adapter {
    mui_display *backend;
    void *buffer;
    int error;
};
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    struct adapter *a = lv_display_get_driver_data(display);
    const mui_surface *s = mui_buffer(a->backend);
    if (area->x1 < 0 || area->y1 < 0 || area->x2 < area->x1 || area->y2 < area->y1 ||
        (uint32_t)area->x2 >= s->width || (uint32_t)area->y2 >= s->height) {
        a->error = EINVAL;
    }
    if (!a->error) {
        size_t bytes = (size_t)(area->x2 - area->x1 + 1) * 4;
        size_t stride = lv_draw_buf_width_to_stride((uint32_t)(area->x2 - area->x1 + 1),
                                                    LV_COLOR_FORMAT_XRGB8888);
        for (int32_t y = area->y1; y <= area->y2; ++y) {
            memcpy((uint8_t *)s->pixels + (size_t)y * s->stride + (size_t)area->x1 * 4,
                   pixels + (size_t)(y - area->y1) * stride, bytes);
        }
        if (lv_display_flush_is_last(display) && mui_present(a->backend)) {
            a->error = errno ? errno : EIO;
        }
    }
    /* A failed present must never deadlock LVGL's renderer. */
    lv_display_flush_ready(display);
}

static void destroy(lv_event_t *event) {
    struct adapter *a = lv_event_get_user_data(event);
    free(a->buffer);
    free(a);
}

lv_display_t *mui_lvgl_create(mui_display *backend) {
    const mui_surface *s = mui_buffer(backend);
    if (!s) {
        errno = EINVAL;
        return NULL;
    }
    struct adapter *a = calloc(1, sizeof(*a));
    if (!a) {
        return NULL;
    }
    size_t stride = lv_draw_buf_width_to_stride(s->width, LV_COLOR_FORMAT_XRGB8888);
    size_t bytes = stride * (s->height < 32 ? s->height : 32);
    a->buffer = malloc(bytes);
    a->backend = backend;
    if (!a->buffer) {
        free(a);
        return NULL;
    }
    lv_display_t *display = lv_display_create((int32_t)s->width, (int32_t)s->height);
    if (!display) {
        free(a->buffer);
        free(a);
        errno = ENOMEM;
        return NULL;
    }
    lv_display_set_driver_data(display, a);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(display, a->buffer, NULL, (uint32_t)bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_display_add_event_cb(display, destroy, LV_EVENT_DELETE, a);
    return display;
}

int mui_lvgl_error(lv_display_t *display) {
    if (!display) {
        return EINVAL;
    }
    struct adapter *a = lv_display_get_driver_data(display);
    return a ? a->error : EINVAL;
}
