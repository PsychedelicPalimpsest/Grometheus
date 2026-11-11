#include "grom_ui_frontend.h"
#include "grom_backend.h"
#include "ghost.h"
#include "ui_theme.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GROM_UI_MAX_SECRET 128
#define GROM_UI_MAX_LABEL_BYTES 128

/* Presentation state and actions belong solely to this frontend. */
typedef enum {
    GROM_SCREEN_AUTH, GROM_SCREEN_AUTH_REJECTED,
    GROM_SCREEN_AUTH_FAILURE, GROM_SCREEN_DESTINATIONS, GROM_SCREEN_CHARGING,
    GROM_SCREEN_PREPARING, GROM_SCREEN_BOOT_FAILURE, GROM_SCREEN_PRESERVE,
    GROM_SCREEN_NEW_SLOT, GROM_SCREEN_NEW_CREDENTIAL, GROM_SCREEN_REVIEW,
    GROM_SCREEN_CREATE_FAILURE, GROM_SCREEN_COMPLETE
} ui_screen;

typedef enum {
    GROM_UI_UNLOCK, GROM_UI_SELECT_SLOT, GROM_UI_SELECT_ADMIN,
    GROM_UI_PRESERVE_CREDENTIAL, GROM_UI_PRESERVE_DONE,
    GROM_UI_SET_SLOT_DETAILS, GROM_UI_SET_SLOT_CREDENTIAL,
    GROM_UI_ACK_OVERWRITE, GROM_UI_CREATE_SLOT, GROM_UI_BACK,
    GROM_UI_CANCEL, GROM_UI_RETRY, GROM_UI_REBOOT, GROM_UI_SHUTDOWN
} ui_action;

typedef struct {
    ui_screen screen;
    grom_charge_status charge;
    size_t destination_count;
    grom_slot_choice choices[GROM_BACKEND_MAX_MATCHES];
    char *destinations[GROM_BACKEND_MAX_MATCHES];
    bool admin_destination;
    size_t preserved_count;
    char *preserved[GROM_BACKEND_MAX_PRESERVED];
    size_t recent_preserved;
    char *new_slot_label;
    uint64_t new_slot_size_gib;
    bool overwrite_acknowledged;
    bool preserve_rejected;
    bool detail_rejected;
} ui_view;

struct grom_ui_frontend {
    ui_view view;
    grom_auth_session auth_session;
    grom_provision_session provision_session;
    lv_timer_t *charge_timer;
    lv_obj_t *root;
    lv_obj_t *keyboard;
    lv_obj_t *mask_label;
    lv_obj_t *primary;
    lv_obj_t *slot_label;
    lv_obj_t *slot_size;
    unsigned active_secret;
    uint8_t secrets[2][GROM_UI_MAX_SECRET];
    size_t secret_len[2];
    char mask[GROM_UI_MAX_SECRET];
    bool keyboard_open;
};

static void secure_clear(void *pointer, size_t size) {
    volatile uint8_t *bytes = pointer;
    while(size--) *bytes++ = 0;
}

static void clear_secrets(grom_ui_frontend *ui) {
    secure_clear(ui->secrets, sizeof(ui->secrets));
    secure_clear(ui->secret_len, sizeof(ui->secret_len));
    secure_clear(ui->mask, sizeof(ui->mask));
    ui->active_secret = 0;
}

static char *copy_text(const char *value) {
    if(!value) return NULL;
    size_t length = 0;
    while(length <= GROM_UI_MAX_LABEL_BYTES && value[length]) length++;
    if(length > GROM_UI_MAX_LABEL_BYTES) return NULL;
    length++;
    char *copy = malloc(length);
    if(copy) memcpy(copy, value, length);
    return copy;
}

static bool parse_gib(const char *text, uint64_t *value) {
    if(!text || !*text) return false;
    uint64_t parsed = 0;
    for(const char *p = text; *p; p++) {
        if(*p < '0' || *p > '9') return false;
        unsigned digit = (unsigned)(*p - '0');
        if(parsed > (UINT64_MAX - digit) / 10) return false;
        parsed = parsed * 10 + digit;
    }
    if(!parsed) return false;
    *value = parsed;
    return true;
}

static void clear_text(char **value) {
    if(!*value) return;
    secure_clear(*value, strlen(*value));
    free(*value);
    *value = NULL;
}

static void scrub_widgets(lv_obj_t *object) {
    uint32_t count = lv_obj_get_child_count(object);
    for(uint32_t i = 0; i < count; i++)
        scrub_widgets(lv_obj_get_child(object, i));
    if(lv_obj_check_type(object, &lv_label_class)) {
        char *text = (char *)lv_label_get_text(object);
        if(text) secure_clear(text, strlen(text));
    } else if(lv_obj_check_type(object, &lv_textarea_class)) {
        char *text = (char *)lv_textarea_get_text(object);
        if(text) secure_clear(text, strlen(text));
    }
}

static void clear_visible_data(grom_ui_frontend *ui) {
    /* LVGL owns text copies as well as the copies below. Clear them before a
     * backend call can hand off to Android or another UI mode. */
    scrub_widgets(ui->root);
    for(size_t i = 0; i < GROM_BACKEND_MAX_MATCHES; i++)
        clear_text(&ui->view.destinations[i]);
    for(size_t i = 0; i < GROM_BACKEND_MAX_PRESERVED; i++)
        clear_text(&ui->view.preserved[i]);
    clear_text(&ui->view.new_slot_label);
    secure_clear(ui->view.choices, sizeof(ui->view.choices));
    ui->view.destination_count = ui->view.preserved_count = 0;
    ui->view.admin_destination = false;
    ui->view.overwrite_acknowledged = false;
    ui->view.new_slot_size_gib = 0;
    ui->view.recent_preserved = 0;
    ui->view.preserve_rejected = false;
    ui->view.detail_rejected = false;
}

static void cancel_sessions(grom_ui_frontend *ui) {
    if(ui->auth_session) {
        grom_backend_cancel_auth(ui->auth_session);
        ui->auth_session = 0;
    }
    if(ui->provision_session) {
        grom_backend_cancel_provisioning(ui->provision_session);
        ui->provision_session = 0;
    }
    clear_visible_data(ui);
    clear_secrets(ui);
}

static lv_obj_t *label(lv_obj_t *parent, const char *value, int x, int y,
                       int width, const lv_font_t *font) {
    lv_obj_t *object = lv_label_create(parent);
    lv_label_set_text(object, value);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_width(object, width);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, UI_RED, 0);
    return object;
}

static lv_obj_t *button(grom_ui_frontend *ui, const char *caption,
                        int x, int y, int width) {
    lv_obj_t *object = lv_button_create(ui->root);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, UI_TARGET);
    lv_obj_set_style_bg_color(object, UI_BG, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(object, UI_RED, 0);
    lv_obj_set_style_border_width(object, UI_STROKE, 0);
    lv_obj_set_style_radius(object, UI_RADIUS, 0);
    lv_obj_set_style_bg_color(object, UI_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_opa(object, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(object, UI_SURFACE, LV_STATE_CHECKED);
    lv_obj_set_style_border_width(object, 4, LV_STATE_CHECKED);
    lv_obj_t *caption_label = lv_label_create(object);
    lv_label_set_text(caption_label, caption);
    lv_obj_set_width(caption_label, width - 20);
    lv_label_set_long_mode(caption_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(caption_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(caption_label, UI_RED, 0);
    lv_obj_set_style_text_font(caption_label, &lv_font_montserrat_18, 0);
    lv_obj_center(caption_label);
    return object;
}

static void render(grom_ui_frontend *ui);

static void refresh_later(void *context) {
    render(context);
}

static void choose_slot(grom_ui_frontend *ui, size_t index) {
    if(!ui->auth_session || index >= ui->view.destination_count) return;
    grom_slot_choice choice = ui->view.choices[index];
    clear_visible_data(ui);
    grom_backend_result result = grom_backend_select_slot(
        ui->auth_session, choice);
    secure_clear(&choice, sizeof(choice));
    if(result != GROM_BACKEND_OK)
        grom_backend_cancel_auth(ui->auth_session);
    ui->auth_session = 0;
    ui->view.screen = result == GROM_BACKEND_OK ?
        GROM_SCREEN_PREPARING : GROM_SCREEN_BOOT_FAILURE;
}

static void choose_admin(grom_ui_frontend *ui) {
    if(!ui->auth_session || !ui->view.admin_destination) return;
    clear_visible_data(ui);
    grom_provision_session next = 0;
    grom_backend_result result = grom_backend_begin_provisioning(
        ui->auth_session, &next);
    if(result != GROM_BACKEND_OK || !next)
        grom_backend_cancel_auth(ui->auth_session);
    ui->auth_session = 0;
    if(result == GROM_BACKEND_OK && next) {
        ui->provision_session = next;
        ui->view.screen = GROM_SCREEN_PRESERVE;
    } else {
        ui->view.screen = GROM_SCREEN_AUTH_FAILURE;
    }
}

static void authenticate(grom_ui_frontend *ui) {
    grom_auth_matches matches = {0};
    grom_backend_result result = grom_backend_authenticate(
        ui->secrets[0], ui->secret_len[0], &matches);
    clear_secrets(ui);
    if(result == GROM_BACKEND_NO_MATCH) {
        if(matches.session)
            grom_backend_cancel_auth(matches.session);
        ui->view.screen = GROM_SCREEN_AUTH_REJECTED;
    } else if(result != GROM_BACKEND_OK || !matches.session ||
              matches.slot_count > GROM_BACKEND_MAX_MATCHES ||
              (!matches.slot_count && !matches.administration)) {
        if(matches.session)
            grom_backend_cancel_auth(matches.session);
        ui->view.screen = GROM_SCREEN_AUTH_FAILURE;
    } else {
        ui->auth_session = matches.session;
        ui->view.destination_count = matches.slot_count;
        ui->view.admin_destination = matches.administration;
        bool copied = true;
        for(size_t i = 0; i < matches.slot_count; i++) {
            ui->view.choices[i] = matches.slots[i].choice;
            ui->view.destinations[i] = copy_text(matches.slots[i].label);
            if(!ui->view.destinations[i]) copied = false;
        }
        secure_clear(&matches, sizeof(matches));
        if(!copied) {
            cancel_sessions(ui);
            ui->view.screen = GROM_SCREEN_AUTH_FAILURE;
        } else if(ui->view.destination_count == 1 &&
                  !ui->view.admin_destination) {
            choose_slot(ui, 0);
        } else if(!ui->view.destination_count &&
                  ui->view.admin_destination) {
            choose_admin(ui);
        } else {
            ui->view.screen = GROM_SCREEN_DESTINATIONS;
        }
    }
    secure_clear(&matches, sizeof(matches));
}

static void preserve(grom_ui_frontend *ui) {
    if(!ui->provision_session) return;
    grom_backend_result result = grom_backend_preserve(
        ui->provision_session,
        ui->secrets[0], ui->secret_len[0]);
    clear_secrets(ui);
    ui->view.preserve_rejected = result == GROM_BACKEND_NO_MATCH;
    if(result == GROM_BACKEND_NO_MATCH) return;
    if(result != GROM_BACKEND_OK) {
        cancel_sessions(ui);
        ui->view.screen = GROM_SCREEN_CREATE_FAILURE;
        return;
    }
    grom_preservation list = {0};
    result = grom_backend_preservation(ui->provision_session, &list);
    if(result != GROM_BACKEND_OK ||
       list.label_count > GROM_BACKEND_MAX_PRESERVED ||
       (list.label_count && list.newest_label >= list.label_count)) {
        cancel_sessions(ui);
        ui->view.screen = GROM_SCREEN_CREATE_FAILURE;
        return;
    }
    for(size_t i = 0; i < GROM_BACKEND_MAX_PRESERVED; i++)
        clear_text(&ui->view.preserved[i]);
    ui->view.preserved_count = list.label_count;
    ui->view.recent_preserved = list.newest_label;
    for(size_t i = 0; i < list.label_count; i++) {
        ui->view.preserved[i] = copy_text(list.labels[i]);
        if(!ui->view.preserved[i]) {
            cancel_sessions(ui);
            ui->view.screen = GROM_SCREEN_CREATE_FAILURE;
            break;
        }
    }
}

static void dispatch(grom_ui_frontend *ui, ui_action action, size_t index) {
    switch(action) {
    case GROM_UI_UNLOCK:
        if(ui->secret_len[0]) authenticate(ui);
        break;
    case GROM_UI_SELECT_SLOT: choose_slot(ui, index); break;
    case GROM_UI_SELECT_ADMIN: choose_admin(ui); break;
    case GROM_UI_PRESERVE_CREDENTIAL:
        if(ui->secret_len[0]) preserve(ui);
        break;
    case GROM_UI_PRESERVE_DONE:
        if(ui->provision_session) ui->view.screen = GROM_SCREEN_NEW_SLOT;
        break;
    case GROM_UI_SET_SLOT_DETAILS: {
        const char *name = ui->slot_label ? lv_textarea_get_text(ui->slot_label) : NULL;
        const char *size = ui->slot_size ? lv_textarea_get_text(ui->slot_size) : NULL;
        uint64_t parsed = 0;
        if(!name || !*name || !parse_gib(size, &parsed)) {
            ui->view.detail_rejected = true;
            break;
        }
        char *copy = copy_text(name);
        if(!copy) {
            ui->view.screen = GROM_SCREEN_CREATE_FAILURE;
            break;
        }
        clear_text(&ui->view.new_slot_label);
        ui->view.new_slot_label = copy;
        ui->view.new_slot_size_gib = parsed;
        ui->view.detail_rejected = false;
        ui->view.screen = GROM_SCREEN_NEW_CREDENTIAL;
        break;
    }
    case GROM_UI_SET_SLOT_CREDENTIAL: {
        if(!ui->provision_session || !ui->view.new_slot_label) break;
        if(!ui->secret_len[0] || ui->secret_len[0] != ui->secret_len[1] ||
           memcmp(ui->secrets[0], ui->secrets[1], ui->secret_len[0])) {
            ui->view.detail_rejected = true;
            break;
        }
        grom_backend_result result = grom_backend_prepare_new_slot(
            ui->provision_session,
            ui->view.new_slot_label, ui->view.new_slot_size_gib,
            ui->secrets[0], ui->secret_len[0]);
        clear_secrets(ui);
        if(result == GROM_BACKEND_OK) {
            ui->view.detail_rejected = false;
            ui->view.screen = GROM_SCREEN_REVIEW;
        } else if(result == GROM_BACKEND_INVALID_INPUT) {
            ui->view.detail_rejected = true;
        } else {
            cancel_sessions(ui);
            ui->view.screen = GROM_SCREEN_CREATE_FAILURE;
        }
        break;
    }
    case GROM_UI_ACK_OVERWRITE:
        ui->view.overwrite_acknowledged = index != 0;
        break;
    case GROM_UI_CREATE_SLOT:
        if(ui->provision_session && ui->view.overwrite_acknowledged) {
            grom_backend_result result = grom_backend_create_slot(
                ui->provision_session, true);
            if(result != GROM_BACKEND_OK)
                grom_backend_cancel_provisioning(ui->provision_session);
            ui->provision_session = 0;
            clear_visible_data(ui);
            ui->view.screen = result == GROM_BACKEND_OK ?
                GROM_SCREEN_COMPLETE : GROM_SCREEN_CREATE_FAILURE;
        }
        break;
    case GROM_UI_BACK:
        if(ui->view.screen == GROM_SCREEN_NEW_SLOT)
            ui->view.screen = GROM_SCREEN_PRESERVE;
        else if(ui->view.screen == GROM_SCREEN_NEW_CREDENTIAL)
            ui->view.screen = GROM_SCREEN_NEW_SLOT;
        else if(ui->view.screen == GROM_SCREEN_REVIEW) {
            grom_backend_discard_new_slot(ui->provision_session);
            ui->view.overwrite_acknowledged = false;
            ui->view.screen = GROM_SCREEN_NEW_CREDENTIAL;
        }
        break;
    case GROM_UI_CANCEL:
        cancel_sessions(ui);
        ui->view.screen = GROM_SCREEN_AUTH;
        break;
    case GROM_UI_RETRY:
        cancel_sessions(ui);
        ui->view.screen = GROM_SCREEN_AUTH;
        break;
    case GROM_UI_REBOOT:
    case GROM_UI_SHUTDOWN:
        if(grom_backend_power(
            action == GROM_UI_REBOOT ? GROM_POWER_REBOOT : GROM_POWER_SHUTDOWN)
            != GROM_BACKEND_OK)
            ui->view.screen = GROM_SCREEN_AUTH_FAILURE;
        break;
    }
    clear_secrets(ui);
    lv_async_call(refresh_later, ui);
}

typedef struct {
    grom_ui_frontend *ui;
    ui_action action;
    size_t index;
} button_action;

static void action_clicked(lv_event_t *event) {
    button_action *binding = lv_event_get_user_data(event);
    dispatch(binding->ui, binding->action, binding->index);
}

static void free_action(lv_event_t *event) {
    free(lv_event_get_user_data(event));
}

static lv_obj_t *action(grom_ui_frontend *ui, const char *caption,
                        int x, int y, int width,
                        ui_action kind, size_t index) {
    lv_obj_t *object = button(ui, caption, x, y, width);
    button_action *binding = malloc(sizeof(*binding));
    if(!binding) {
        lv_obj_add_state(object, LV_STATE_DISABLED);
        return object;
    }
    *binding = (button_action){.ui = ui, .action = kind, .index = index};
    lv_obj_add_event_cb(object, action_clicked, LV_EVENT_CLICKED, binding);
    lv_obj_add_event_cb(object, free_action, LV_EVENT_DELETE, binding);
    return object;
}

static void update_mask(grom_ui_frontend *ui) {
    if(!ui->mask_label) return;
    size_t count = ui->secret_len[ui->active_secret];
    if(count > 16) count = 16;
    memset(ui->mask, '*', count);
    ui->mask[count] = 0;
    lv_label_set_text(ui->mask_label, count ? ui->mask : "Password");
    if(ui->primary) {
        lv_obj_set_state(ui->primary, LV_STATE_DISABLED,
                         ui->secret_len[0] == 0);
    }
}

static void keyboard_value(lv_event_t *event) {
    grom_ui_frontend *ui = lv_event_get_user_data(event);
    if(lv_event_get_code(event) == LV_EVENT_READY ||
       lv_event_get_code(event) == LV_EVENT_CANCEL) {
        lv_obj_add_flag(ui->keyboard, LV_OBJ_FLAG_HIDDEN);
        ui->keyboard_open = false;
        return;
    }
    if(lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED ||
       lv_keyboard_get_textarea(ui->keyboard)) return;
    uint32_t key = lv_buttonmatrix_get_selected_button(ui->keyboard);
    if(key == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *value = lv_buttonmatrix_get_button_text(ui->keyboard, key);
    if(!value) return;
    size_t *length = &ui->secret_len[ui->active_secret];
    if(strcmp(value, LV_SYMBOL_BACKSPACE) == 0) {
        if(*length) ui->secrets[ui->active_secret][--*length] = 0;
    } else if(strcmp(value, "abc") && strcmp(value, "ABC") &&
              strcmp(value, "1#") && strcmp(value, LV_SYMBOL_LEFT) &&
              strcmp(value, LV_SYMBOL_RIGHT) &&
              strcmp(value, LV_SYMBOL_NEW_LINE) &&
              strcmp(value, LV_SYMBOL_KEYBOARD) &&
              strcmp(value, LV_SYMBOL_OK)) {
        size_t added = strlen(value);
        if(added && added <= 4 && *length + added < GROM_UI_MAX_SECRET) {
            memcpy(ui->secrets[ui->active_secret] + *length, value, added);
            *length += added;
            ui->secrets[ui->active_secret][*length] = 0;
        }
    }
    update_mask(ui);
}

static void open_secret(lv_event_t *event) {
    grom_ui_frontend *ui = lv_event_get_user_data(event);
    ui->active_secret = (unsigned)(uintptr_t)lv_obj_get_user_data(
        lv_event_get_current_target_obj(event));
    ui->mask_label = lv_obj_get_child(lv_event_get_current_target_obj(event), 0);
    lv_keyboard_set_textarea(ui->keyboard, NULL);
    lv_obj_remove_flag(ui->keyboard, LV_OBJ_FLAG_HIDDEN);
    ui->keyboard_open = true;
    update_mask(ui);
}

static lv_obj_t *secret_field(grom_ui_frontend *ui, const char *placeholder,
                              int x, int y, int width, unsigned index) {
    lv_obj_t *object = lv_obj_create(ui->root);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, UI_TARGET);
    lv_obj_set_style_bg_color(object, UI_BG, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(object, UI_RED, 0);
    lv_obj_set_style_border_width(object, UI_STROKE, 0);
    lv_obj_set_style_border_side(object, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_user_data(object, (void *)(uintptr_t)index);
    lv_obj_add_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *caption = label(object, placeholder, 12, 12,
                              width - 24, &lv_font_montserrat_24);
    lv_obj_set_style_text_color(caption, UI_DIM, 0);
    lv_obj_add_event_cb(object, open_secret, LV_EVENT_CLICKED, ui);
    return object;
}

static void plain_focus(lv_event_t *event) {
    grom_ui_frontend *ui = lv_event_get_user_data(event);
    lv_keyboard_set_textarea(ui->keyboard, lv_event_get_current_target_obj(event));
    lv_obj_remove_flag(ui->keyboard, LV_OBJ_FLAG_HIDDEN);
    ui->keyboard_open = true;
}

static lv_obj_t *plain_field(grom_ui_frontend *ui, const char *placeholder,
                             int x, int y, int width) {
    lv_obj_t *object = lv_textarea_create(ui->root);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, UI_TARGET);
    lv_textarea_set_one_line(object, true);
    lv_textarea_set_max_length(object, GROM_UI_MAX_LABEL_BYTES);
    lv_textarea_set_placeholder_text(object, placeholder);
    lv_obj_set_style_bg_color(object, UI_BG, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(object, UI_RED, 0);
    lv_obj_set_style_text_font(object, &lv_font_montserrat_24, 0);
    lv_obj_set_style_border_color(object, UI_RED, 0);
    lv_obj_set_style_border_width(object, UI_STROKE, 0);
    lv_obj_set_style_border_side(object, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(object, 0, 0);
    lv_obj_add_event_cb(object, plain_focus, LV_EVENT_CLICKED, ui);
    return object;
}

static void make_keyboard(grom_ui_frontend *ui, bool visible) {
    ui->keyboard = lv_keyboard_create(ui->root);
    lv_obj_set_size(ui->keyboard, UI_W, 304);
    lv_obj_set_style_bg_color(ui->keyboard, UI_BG, 0);
    lv_obj_set_style_bg_color(ui->keyboard, UI_SURFACE, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(ui->keyboard, UI_SURFACE,
                              LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(ui->keyboard, UI_PRESSED,
                              LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(ui->keyboard, UI_RED, LV_PART_ITEMS);
    lv_obj_set_style_text_color(ui->keyboard, UI_RED,
                                LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_font(ui->keyboard, &lv_font_montserrat_18,
                               LV_PART_ITEMS);
    lv_obj_set_style_radius(ui->keyboard, 0, LV_PART_ITEMS);
    lv_obj_add_event_cb(ui->keyboard, keyboard_value, LV_EVENT_ALL, ui);
    if(!visible) lv_obj_add_flag(ui->keyboard, LV_OBJ_FLAG_HIDDEN);
    ui->keyboard_open = visible;
}

static void page(grom_ui_frontend *ui, const char *title, const char *body) {
    label(ui->root, title, UI_PAD, 96, UI_W - 2*UI_PAD,
          &lv_font_montserrat_32);
    if(body && *body) label(ui->root, body, UI_PAD, 160,
                            UI_W - 2*UI_PAD, &lv_font_montserrat_24);
}

static void preserved_list(grom_ui_frontend *ui) {
    lv_obj_t *list = lv_obj_create(ui->root);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, UI_PAD, UI_PRESERVED_TOP);
    lv_obj_set_size(list, UI_W - 2*UI_PAD, UI_PRESERVED_HEIGHT);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(list, UI_BG, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(list, UI_RED, LV_PART_SCROLLBAR);
    lv_obj_t *recent = NULL;
    size_t count = ui->view.preserved_count;
    if(count > GROM_BACKEND_MAX_PRESERVED) count = GROM_BACKEND_MAX_PRESERVED;
    for(size_t index = 0; index < count; index++) {
        if(!ui->view.preserved[index]) continue;
        lv_obj_t *entry = label(list, ui->view.preserved[index], 0,
                                (int)index*UI_PRESERVED_ROW,
                                UI_W - 2*UI_PAD - 2*UI_GAP,
                                &lv_font_montserrat_24);
        lv_obj_set_height(entry, UI_PRESERVED_ROW);
        lv_label_set_long_mode(entry, LV_LABEL_LONG_DOT);
        if(index == ui->view.recent_preserved) recent = entry;
    }
    lv_obj_update_layout(list);
    if(recent) lv_obj_scroll_to_view(recent, LV_ANIM_OFF);
}

static void destination_list(grom_ui_frontend *ui) {
    lv_obj_t *list = lv_obj_create(ui->root);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 32, 224);
    lv_obj_set_size(list, 416, 496);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(list, UI_BG, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(list, UI_RED, LV_PART_SCROLLBAR);
    size_t count = ui->view.destination_count;
    if(count > GROM_BACKEND_MAX_MATCHES) count = GROM_BACKEND_MAX_MATCHES;
    int row = 0;
    for(size_t i = 0; i < count; i++) {
        if(!ui->view.destinations[i]) continue;
        lv_obj_t *entry = action(ui, ui->view.destinations[i], 0,
                                 row++ * 72, 416, GROM_UI_SELECT_SLOT, i);
        lv_obj_set_parent(entry, list);
    }
    if(ui->view.admin_destination) {
        lv_obj_t *entry = action(ui, "Administration", 0, row * 72,
                                 416, GROM_UI_SELECT_ADMIN, 0);
        lv_obj_set_parent(entry, list);
    }
}

static void ack_changed(lv_event_t *event) {
    grom_ui_frontend *ui = lv_event_get_user_data(event);
    dispatch(ui, GROM_UI_ACK_OVERWRITE,
             lv_obj_has_state(lv_event_get_current_target_obj(event),
                              LV_STATE_CHECKED) ? 1u : 0u);
}

static void render(grom_ui_frontend *ui) {
    ui->keyboard = ui->mask_label = ui->primary = NULL;
    ui->slot_label = ui->slot_size = NULL;
    scrub_widgets(ui->root);
    lv_obj_clean(ui->root);
    switch(ui->view.screen) {
    case GROM_SCREEN_AUTH:
    case GROM_SCREEN_AUTH_REJECTED: {
        lv_obj_t *ghost = lv_image_create(ui->root);
        lv_image_set_src(ghost, grom_ghost_image());
        lv_image_set_antialias(ghost, false);
        lv_obj_set_pos(ghost, 120, 64);
        lv_obj_t *field = secret_field(ui, "Password", 104, 424, 272, 0);
        ui->primary = action(ui, "Unlock", 160, 504, 160, GROM_UI_UNLOCK, 0);
        lv_obj_add_state(ui->primary, LV_STATE_DISABLED);
        make_keyboard(ui, false);
        ui->mask_label = lv_obj_get_child(field, 0);
        if(ui->view.screen == GROM_SCREEN_AUTH_REJECTED)
            label(ui->root, "Unable to unlock. Try again.", 80, 568, 340,
                  &lv_font_montserrat_18);
        break;
    }
    case GROM_SCREEN_CHARGING: {
        label(ui->root, LV_SYMBOL_CHARGE, 208, 352, 64,
              &lv_font_montserrat_32);
        if(ui->view.charge.percent_known && ui->view.charge.percent <= 100) {
            char percentage[8];
            snprintf(percentage, sizeof(percentage), "%u%%",
                     ui->view.charge.percent);
            label(ui->root, percentage, 190, 416, 100,
                  &lv_font_montserrat_32);
        }
        break;
    }
    case GROM_SCREEN_DESTINATIONS: {
        page(ui, "Choose destination", NULL);
        destination_list(ui);
        action(ui, "Cancel", 32, 780, 416, GROM_UI_CANCEL, 0);
        break;
    }
    case GROM_SCREEN_PRESERVE:
        label(ui->root, "Preserve existing slots", 32, 64, 416,
              &lv_font_montserrat_32);
        label(ui->root, "Add passwords, or continue with none.", 32, 120, 416,
              &lv_font_montserrat_18);
        label(ui->root, ui->view.preserved_count ?
              "Will be preserved" : "No slots added", 32, 168, 416,
              &lv_font_montserrat_18);
        if(ui->view.preserve_rejected)
            label(ui->root, "Unable to unlock. Try again.", 32, 320, 416,
                  &lv_font_montserrat_18);
        preserved_list(ui);
        secret_field(ui, "Existing password", 32, 360, 416, 0);
        ui->primary = action(ui, "Add", 32, 440, 200,
                             GROM_UI_PRESERVE_CREDENTIAL, 0);
        lv_obj_add_state(ui->primary, LV_STATE_DISABLED);
        action(ui, "Continue", 248, 440, 200, GROM_UI_PRESERVE_DONE, 0);
        action(ui, "Cancel", 32, 512, 416, GROM_UI_CANCEL, 0);
        make_keyboard(ui, true);
        break;
    case GROM_SCREEN_NEW_SLOT:
        page(ui, "New slot", NULL);
        if(ui->view.detail_rejected)
            label(ui->root, "Check the label and size.", 32, 160, 416,
                  &lv_font_montserrat_18);
        ui->slot_label = plain_field(ui, "Label", 32, 224, 416);
        ui->slot_size = plain_field(ui, "Size (GiB)", 32, 312, 416);
        action(ui, "Back", 32, 440, 200, GROM_UI_BACK, 0);
        action(ui, "Continue", 248, 440, 200, GROM_UI_SET_SLOT_DETAILS, 0);
        make_keyboard(ui, false);
        break;
    case GROM_SCREEN_NEW_CREDENTIAL:
        page(ui, "New password", "Use a strong passphrase.");
        if(ui->view.detail_rejected)
            label(ui->root, "Check the password and confirmation.",
                  32, 216, 416, &lv_font_montserrat_18);
        secret_field(ui, "Password", 32, 272, 416, 0);
        secret_field(ui, "Confirm", 32, 360, 416, 1);
        action(ui, "Back", 32, 472, 200, GROM_UI_BACK, 0);
        action(ui, "Continue", 248, 472, 200,
               GROM_UI_SET_SLOT_CREDENTIAL, 0);
        make_keyboard(ui, false);
        break;
    case GROM_SCREEN_REVIEW: {
        char summary[128];
        snprintf(summary, sizeof(summary), "%s / %" PRIu64 " GiB",
                 ui->view.new_slot_label ? ui->view.new_slot_label : "",
                 ui->view.new_slot_size_gib);
        page(ui, "Create slot?", summary);
        label(ui->root,
              "Slots whose passwords you\ndid not enter may be overwritten.\n\n"
              "Remaining space is not\nverified free space.",
              32, 264, 416, &lv_font_montserrat_24);
        lv_obj_t *ack = button(ui, "I understand the risk", 32, 536, 416);
        lv_obj_add_flag(ack, LV_OBJ_FLAG_CHECKABLE);
        if(ui->view.overwrite_acknowledged)
            lv_obj_add_state(ack, LV_STATE_CHECKED);
        lv_obj_add_event_cb(ack, ack_changed, LV_EVENT_VALUE_CHANGED, ui);
        action(ui, "Back", 32, 704, 200, GROM_UI_BACK, 0);
        ui->primary = action(ui, "Create", 248, 704, 200,
                             GROM_UI_CREATE_SLOT, 0);
        /* Backend independently requires acknowledgement in create_slot. */
        lv_obj_set_state(ui->primary, LV_STATE_DISABLED,
                         !ui->view.overwrite_acknowledged);
        break;
    }
    case GROM_SCREEN_PREPARING:
        page(ui, "Starting slot...", NULL);
        break;
    case GROM_SCREEN_BOOT_FAILURE:
        page(ui, "Unable to start slot", "Try again or shut down.");
        action(ui, "Retry", 32, 704, 200, GROM_UI_RETRY, 0);
        action(ui, "Shut down", 248, 704, 200, GROM_UI_SHUTDOWN, 0);
        break;
    case GROM_SCREEN_AUTH_FAILURE:
        page(ui, "Preboot unavailable", "Try again or shut down.");
        action(ui, "Retry", 32, 704, 200, GROM_UI_RETRY, 0);
        action(ui, "Shut down", 248, 704, 200, GROM_UI_SHUTDOWN, 0);
        break;
    case GROM_SCREEN_CREATE_FAILURE:
        page(ui, "Provisioning unavailable", "Stay in preboot.");
        action(ui, "Shut down", 32, 704, 416, GROM_UI_SHUTDOWN, 0);
        break;
    case GROM_SCREEN_COMPLETE:
        page(ui, "Slot created", NULL);
        action(ui, "Reboot", 32, 704, 416, GROM_UI_REBOOT, 0);
        break;
    }
}

grom_ui_frontend *grom_ui_create(lv_display_t *display) {
    if(!display) return NULL;
    grom_ui_frontend *ui = calloc(1, sizeof(*ui));
    if(!ui) return NULL;
    ui->root = lv_display_get_screen_active(display);
    lv_obj_set_style_bg_color(ui->root, UI_BG, 0);
    lv_obj_remove_flag(ui->root, LV_OBJ_FLAG_SCROLLABLE);
    return ui;
}

int grom_ui_show_password(grom_ui_frontend *ui) {
    if(!ui) return -1;
    lv_async_call_cancel(refresh_later, ui);
    if(ui->charge_timer) {
        lv_timer_delete(ui->charge_timer);
        ui->charge_timer = NULL;
    }
    cancel_sessions(ui);
    ui->view.screen = GROM_SCREEN_AUTH;
    render(ui);
    return 0;
}

static void charge_tick(lv_timer_t *timer) {
    grom_ui_frontend *ui = lv_timer_get_user_data(timer);
    grom_charge_status next = {0};
    if(grom_backend_charge_status(&next) != GROM_BACKEND_OK)
        next = (grom_charge_status){0};
    if(next.percent_known && next.percent > 100) next.percent_known = false;
    if(next.percent_known != ui->view.charge.percent_known ||
       next.percent != ui->view.charge.percent) {
        ui->view.charge = next;
        render(ui);
    }
}

int grom_ui_show_charging(grom_ui_frontend *ui) {
    if(!ui) return -1;
    lv_async_call_cancel(refresh_later, ui);
    cancel_sessions(ui);
    ui->view.screen = GROM_SCREEN_CHARGING;
    ui->view.charge = (grom_charge_status){0};
    if(!ui->charge_timer) {
        ui->charge_timer = lv_timer_create(charge_tick, 5000, ui);
        if(!ui->charge_timer) return -1;
    }
    charge_tick(ui->charge_timer);
    render(ui);
    return 0;
}

void grom_ui_destroy(grom_ui_frontend *ui) {
    if(!ui) return;
    lv_async_call_cancel(refresh_later, ui);
    if(ui->charge_timer) lv_timer_delete(ui->charge_timer);
    cancel_sessions(ui);
    scrub_widgets(ui->root);
    lv_obj_clean(ui->root);
    free(ui);
}
