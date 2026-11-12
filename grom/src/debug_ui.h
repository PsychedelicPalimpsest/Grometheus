#ifndef GROM_DEBUG_UI_H
#define GROM_DEBUG_UI_H

#include <stdbool.h>
#include "gromui.h"
#include "gromui_lvgl.h"
#include "debug_ui_protocol.h"

typedef struct {
    int listener;
    bool injecting;
    bool pressed;
    int32_t x;
    int32_t y;
} grom_debug_ui;

void grom_debug_ui_start(grom_debug_ui *debug);
void grom_debug_ui_stop(grom_debug_ui *debug);
void grom_debug_ui_poll(grom_debug_ui *debug, lv_indev_t *indev,
                        lv_display_t *display, gromui_display *backend);

#endif
