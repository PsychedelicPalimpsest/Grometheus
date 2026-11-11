#include "grom_ui_stub.h"

#include <string.h>

static int snapshot(void *context, grom_ui_view *out) {
    const grom_ui_stub *stub = context;
    memset(out, 0, sizeof(*out));
    out->screen = stub->screen;
    return 0;
}

static int handle(void *context, const grom_ui_request *request) {
    grom_ui_stub *stub = context;
    switch (request->action) {
    case GROM_UI_UNLOCK:
        /* Generic failure only. No credential inspection or logging. */
        stub->screen = GROM_SCREEN_AUTH_REJECTED;
        return 0;
    case GROM_UI_RETRY:
    case GROM_UI_CANCEL:
        stub->screen = GROM_SCREEN_AUTH;
        return 0;
    default:
        return -1;
    }
}

void grom_ui_stub_init(grom_ui_stub *stub) {
    stub->screen = GROM_SCREEN_AUTH;
}

grom_ui_backend grom_ui_stub_backend(grom_ui_stub *stub) {
    return (grom_ui_backend){ .context = stub, .snapshot = snapshot, .handle = handle };
}
