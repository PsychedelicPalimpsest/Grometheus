#ifndef GROM_DEBUG_UI_PROTOCOL_H
#define GROM_DEBUG_UI_PROTOCOL_H

#include <stdint.h>

#define GROM_DEBUG_UI_SOCKET "/grom/ui-debug.sock"
#define GROM_DEBUG_UI_MAGIC 0x47444247u

enum grom_debug_ui_operation {
    GROM_DEBUG_UI_TAP = 1,
    GROM_DEBUG_UI_SCREENSHOT = 2,
};

typedef struct {
    uint32_t magic;
    uint32_t operation;
    uint32_t x;
    uint32_t y;
} grom_debug_ui_request;

#endif
