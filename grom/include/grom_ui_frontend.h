#ifndef GROM_UI_FRONTEND_H
#define GROM_UI_FRONTEND_H

#include "grom_ui_contract.h"
#include "lvgl.h"

/* Frontend module boundary. An alternative design implements these same
 * functions; grom's authentication/provisioning backend stays unchanged.
 * The owner initializes LVGL/display/input, drives lv_timer_handler, and
 * destroys the frontend before deleting the LVGL display. */
typedef struct grom_ui_frontend grom_ui_frontend;
typedef struct {
    grom_ui_frontend *(*create)(lv_display_t *, grom_ui_backend);
    int (*refresh)(grom_ui_frontend *);
    void (*destroy)(grom_ui_frontend *);
} grom_ui_frontend_ops;

extern const grom_ui_frontend_ops grom_outline_frontend;

#endif
