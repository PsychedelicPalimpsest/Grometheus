#ifndef GROM_UI_FRONTEND_H
#define GROM_UI_FRONTEND_H

#include "lvgl.h"

/* The owner initializes LVGL/display/input, chooses password or charging,
 * and destroys the frontend before deleting the LVGL display. All later
 * screens are private to the frontend. */
typedef struct grom_ui_frontend grom_ui_frontend;
grom_ui_frontend *grom_ui_create(lv_display_t *);
int grom_ui_show_password(grom_ui_frontend *);
int grom_ui_show_charging(grom_ui_frontend *);
void grom_ui_destroy(grom_ui_frontend *);

#endif
