#include "grom_ui_frontend.h"
#include "ghost.h"
#include "ui_theme.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct grom_ui_frontend {
    lv_display_t *display;
    grom_ui_backend backend;
    grom_ui_view view;
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
    lv_label_set_long_mode(caption_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(caption_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(caption_label, UI_RED, 0);
    lv_obj_set_style_text_font(caption_label, &lv_font_montserrat_18, 0);
    lv_obj_center(caption_label);
    return object;
}

static void refresh_later(void *context) {
    (void)grom_outline_frontend.refresh(context);
}

static void dispatch(grom_ui_frontend *ui, grom_ui_action action, size_t index) {
    grom_ui_request request = {.action = action, .index = index};
    switch(action) {
    case GROM_UI_UNLOCK:
    case GROM_UI_PRESERVE_CREDENTIAL:
        request.credential = ui->secrets[0];
        request.credential_len = ui->secret_len[0];
        break;
    case GROM_UI_SET_SLOT_CREDENTIAL:
    case GROM_UI_SET_ADMIN_PIN:
        request.credential = ui->secrets[0];
        request.credential_len = ui->secret_len[0];
        request.confirmation = ui->secrets[1];
        request.confirmation_len = ui->secret_len[1];
        break;
    case GROM_UI_SET_SLOT_DETAILS:
        request.label = ui->slot_label ? lv_textarea_get_text(ui->slot_label) : NULL;
        request.size_gib = ui->slot_size ? lv_textarea_get_text(ui->slot_size) : NULL;
        break;
    default:
        break;
    }
    (void)ui->backend.handle(ui->backend.context, &request);
    clear_secrets(ui);
    lv_async_call(refresh_later, ui);
}

typedef struct {
    grom_ui_frontend *ui;
    grom_ui_action action;
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
                        grom_ui_action kind, size_t index) {
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
    if(count > GROM_UI_MAX_PRESERVED) count = GROM_UI_MAX_PRESERVED;
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
    if(count > GROM_UI_MAX_DESTINATIONS) count = GROM_UI_MAX_DESTINATIONS;
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
    lv_obj_clean(ui->root);
    switch(ui->view.screen) {
    case GROM_SCREEN_AUTH:
    case GROM_SCREEN_AUTH_CHECKING:
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
        if(ui->view.screen == GROM_SCREEN_AUTH_CHECKING) {
            lv_obj_remove_flag(field, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_opa(field, LV_OPA_50, 0);
        }
        if(ui->view.screen == GROM_SCREEN_AUTH_REJECTED)
            label(ui->root, "Unable to unlock. Try again.", 80, 568, 340,
                  &lv_font_montserrat_18);
        break;
    }
    case GROM_SCREEN_CHARGING: {
        label(ui->root, LV_SYMBOL_CHARGE, 208, 352, 64,
              &lv_font_montserrat_32);
        if(ui->view.charge_known && ui->view.charge_percent <= 100) {
            char percentage[8];
            snprintf(percentage, sizeof(percentage), "%u%%",
                     ui->view.charge_percent);
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
        ui->slot_label = plain_field(ui, "Label", 32, 224, 416);
        ui->slot_size = plain_field(ui, "Size (GiB)", 32, 312, 416);
        action(ui, "Back", 32, 440, 200, GROM_UI_BACK, 0);
        action(ui, "Continue", 248, 440, 200, GROM_UI_SET_SLOT_DETAILS, 0);
        make_keyboard(ui, false);
        break;
    case GROM_SCREEN_NEW_CREDENTIAL:
    case GROM_SCREEN_ADMIN_SETUP:
        page(ui, ui->view.screen == GROM_SCREEN_ADMIN_SETUP ?
             "Administration PIN" : "New password",
             ui->view.screen == GROM_SCREEN_ADMIN_SETUP ?
             "Protects provisioning.\nDoes not unlock slot data." :
             "Use a strong passphrase.");
        secret_field(ui, ui->view.screen == GROM_SCREEN_ADMIN_SETUP ?
                     "PIN" : "Password", 32, 272, 416, 0);
        secret_field(ui, "Confirm", 32, 360, 416, 1);
        action(ui, "Back", 32, 472, 200, GROM_UI_BACK, 0);
        action(ui, "Continue", 248, 472, 200,
               ui->view.screen == GROM_SCREEN_ADMIN_SETUP ?
               GROM_UI_SET_ADMIN_PIN : GROM_UI_SET_SLOT_CREDENTIAL, 0);
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
        /* Backend must gate Create; the local checkbox alone cannot grant it. */
        lv_obj_set_state(ui->primary, LV_STATE_DISABLED,
                         !ui->view.create_enabled);
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
    case GROM_SCREEN_CREATING:
        page(ui, "Creating slot...", "Keep the device powered.");
        break;
    case GROM_SCREEN_CREATE_FAILURE:
        page(ui, "Creation interrupted", "Stay in preboot.");
        action(ui, "Shut down", 32, 704, 416, GROM_UI_SHUTDOWN, 0);
        break;
    case GROM_SCREEN_COMPLETE:
        page(ui, "Slot created", NULL);
        action(ui, "Reboot", 32, 704, 416, GROM_UI_REBOOT, 0);
        break;
    case GROM_SCREEN_POWER_CONFIRM:
        page(ui, "Shut down?", NULL);
        action(ui, "Cancel", 32, 704, 200, GROM_UI_CANCEL, 0);
        action(ui, "Shut down", 248, 704, 200, GROM_UI_SHUTDOWN, 0);
        break;
    }
}

static grom_ui_frontend *create(lv_display_t *display, grom_ui_backend backend) {
    if(!display || !backend.snapshot || !backend.handle) return NULL;
    grom_ui_frontend *ui = calloc(1, sizeof(*ui));
    if(!ui) return NULL;
    ui->display = display;
    ui->backend = backend;
    ui->root = lv_display_get_screen_active(display);
    lv_obj_set_style_bg_color(ui->root, UI_BG, 0);
    lv_obj_remove_flag(ui->root, LV_OBJ_FLAG_SCROLLABLE);
    if(grom_outline_frontend.refresh(ui) != 0) {
        free(ui);
        return NULL;
    }
    return ui;
}

static int refresh(grom_ui_frontend *ui) {
    grom_ui_view next;
    if(!ui || ui->backend.snapshot(ui->backend.context, &next) != 0) return -1;
    clear_secrets(ui);
    ui->view = next;
    render(ui);
    return 0;
}

static void destroy(grom_ui_frontend *ui) {
    if(!ui) return;
    lv_async_call_cancel(refresh_later, ui);
    clear_secrets(ui);
    lv_obj_clean(ui->root);
    free(ui);
}

const grom_ui_frontend_ops grom_outline_frontend = {
    .create = create, .refresh = refresh, .destroy = destroy
};
