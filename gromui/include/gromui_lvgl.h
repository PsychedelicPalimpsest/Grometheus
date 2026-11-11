/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GROMUI_LVGL_H
#define GROMUI_LVGL_H

#include "lvgl.h"
#include "gromui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Call lv_init first. The caller owns backend and keeps it open until after
 * lv_display_delete. LVGL owns the adapter's partial draw buffer after create.
 * Serialize all LVGL calls and supply a tick callback in the application. */
lv_display_t *gromui_lvgl_create(gromui_display *backend);

/* Sticky errno from flush/present, or zero until failure. Recreate to recover. */
int gromui_lvgl_error(lv_display_t *display);
unsigned gromui_lvgl_presents(lv_display_t *display);

#ifdef __cplusplus
}
#endif

#endif /* GROMUI_LVGL_H */
