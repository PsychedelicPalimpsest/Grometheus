/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GROM_TOUCH_H
#define GROM_TOUCH_H

#include <stdbool.h>
#include <stdint.h>
#include <linux/input.h>

#define GROM_TOUCH_SLOTS 64

typedef struct {
    int32_t x, y;
    bool pressed;
} grom_touch_point;

typedef enum {
    GROM_TOUCH_NONE,
    GROM_TOUCH_SINGLE,
    GROM_TOUCH_MT_A,
    GROM_TOUCH_MT_B,
} grom_touch_protocol;

typedef struct {
    bool active, has_x, has_y;
    bool x_this_frame, y_this_frame;
    int32_t x, y, tracking_id;
} grom_touch_contact;

/* Renderer-owned input state. No display-library or LVGL dependency. */
typedef struct {
    int fd;
    uint32_t width, height;
    uint32_t last_event_ms;
    uint32_t dropped_frames;
    grom_touch_protocol protocol;
    struct input_absinfo axis_x, axis_y;
    grom_touch_contact slots[GROM_TOUCH_SLOTS];
    grom_touch_contact packet, first_packet;
    int current_slot, primary_slot, primary_id;
    int32_t single_x, single_y;
    unsigned slot_count, packet_count;
    bool single_down, any_packet, has_btn_touch;
    bool blocked, dropping, resync_required, cancel_pending;
    grom_touch_point point;
} grom_touch;

void grom_touch_init(grom_touch *touch, uint32_t width, uint32_t height);
void grom_touch_close(grom_touch *touch);
/* Opens an existing direct touchscreen. Call again after coldboot or removal. */
int grom_touch_discover(grom_touch *touch);
/* One complete input frame per result; returns 1 for a changed pointer, 0 if
 * no complete frame is available, and -1 when the device was lost. */
int grom_touch_read(grom_touch *touch, grom_touch_point *point);
/* The frame decoder is also used by synthetic-event tests. */
int grom_touch_feed(grom_touch *touch, const struct input_event *event,
                    grom_touch_point *point);

#endif
