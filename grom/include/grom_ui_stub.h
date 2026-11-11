#ifndef GROM_UI_STUB_H
#define GROM_UI_STUB_H
#include "grom_ui_contract.h"

/* Deliberately inert grom logic stub for UI integration. It cannot authenticate,
 * reveal destinations, preserve ranges, create slots, reboot, or boot Android. */
typedef struct {
    grom_screen screen;
} grom_ui_stub;

void grom_ui_stub_init(grom_ui_stub *stub);
grom_ui_backend grom_ui_stub_backend(grom_ui_stub *stub);
#endif
