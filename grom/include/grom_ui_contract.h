#ifndef GROM_UI_CONTRACT_H
#define GROM_UI_CONTRACT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* This contract has no LVGL or display dependency. Only authenticated labels
 * may appear in destinations/preserved. Pointers in a view are borrowed until
 * the next backend call; the UI copies visible labels into LVGL widgets. */
/* Presentation capacity only; these do not define an on-disk slot count. */
#define GROM_UI_MAX_DESTINATIONS 128
#define GROM_UI_MAX_PRESERVED 128
#define GROM_UI_MAX_SECRET 128

typedef enum {
    GROM_SCREEN_AUTH,
    GROM_SCREEN_AUTH_CHECKING,
    GROM_SCREEN_AUTH_REJECTED,
    GROM_SCREEN_DESTINATIONS,
    GROM_SCREEN_CHARGING,
    GROM_SCREEN_PREPARING,
    GROM_SCREEN_BOOT_FAILURE,
    GROM_SCREEN_PRESERVE,
    GROM_SCREEN_NEW_SLOT,
    GROM_SCREEN_NEW_CREDENTIAL,
    GROM_SCREEN_REVIEW,
    GROM_SCREEN_CREATING,
    GROM_SCREEN_CREATE_FAILURE,
    GROM_SCREEN_COMPLETE,
    GROM_SCREEN_POWER_CONFIRM,
    GROM_SCREEN_ADMIN_SETUP
} grom_screen;

typedef struct {
    grom_screen screen;
    bool charge_known;
    uint8_t charge_percent;
    size_t destination_count;
    const char *destinations[GROM_UI_MAX_DESTINATIONS];
    bool admin_destination;
    size_t preserved_count;
    const char *preserved[GROM_UI_MAX_PRESERVED];
    size_t recent_preserved;
    const char *new_slot_label;
    uint64_t new_slot_size_gib;
    bool overwrite_acknowledged;
    bool create_enabled;
} grom_ui_view;

typedef enum {
    GROM_UI_UNLOCK,
    GROM_UI_SELECT_SLOT,
    GROM_UI_SELECT_ADMIN,
    GROM_UI_PRESERVE_CREDENTIAL,
    GROM_UI_PRESERVE_DONE,
    GROM_UI_SET_SLOT_DETAILS,
    GROM_UI_SET_SLOT_CREDENTIAL,
    GROM_UI_SET_ADMIN_PIN,
    GROM_UI_ACK_OVERWRITE,
    GROM_UI_CREATE_SLOT,
    GROM_UI_BACK,
    GROM_UI_CANCEL,
    GROM_UI_RETRY,
    GROM_UI_REBOOT,
    GROM_UI_SHUTDOWN
} grom_ui_action;

typedef struct {
    grom_ui_action action;
    size_t index;
    const uint8_t *credential;
    size_t credential_len;
    const uint8_t *confirmation;
    size_t confirmation_len;
    const char *label;
    const char *size_gib;
} grom_ui_request;

typedef struct {
    void *context;
    /* Return a current view. No secrets or global slot inventory in it. */
    int (*snapshot)(void *context, grom_ui_view *out);
    /* Synchronous: consume secret pointers before returning. Never retain them.
     * Return 0 if the backend handled the request; nonzero means unavailable.
     * The UI erases its input buffers immediately after this call. */
    int (*handle)(void *context, const grom_ui_request *request);
} grom_ui_backend;

#endif
