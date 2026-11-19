#include "grom_ui_frontend.h"
#include "grom_backend.h"

#undef NDEBUG
#include <assert.h>
#include <ctype.h>
#include <string.h>

/* Exercise the frontend against deterministic backend results. The shipped
 * backend remains the inert stub; this test links replacements only here. */
static enum { NO_MATCH, MULTIPLE, SINGLE, ADMIN_ONLY, INTERNAL_ERROR } mode;
static unsigned authentications, selections, administration, auth_cancels;
static unsigned provision_cancels, charge_queries;

grom_backend_result grom_backend_authenticate(const uint8_t *secret,
                                              size_t length,
                                              grom_auth_matches *out) {
    authentications++;
    assert(secret && length == 1);
    memset(out, 0, sizeof(*out));
    if(mode == NO_MATCH) return GROM_BACKEND_NO_MATCH;
    if(mode == INTERNAL_ERROR) return GROM_BACKEND_RETRYABLE_ERROR;
    out->session = 7;
    if(mode != ADMIN_ONLY) {
        out->slot_count = mode == MULTIPLE ? 2 : 1;
        out->slots[0] = (grom_match){11, "alpha"};
        if(mode == MULTIPLE) out->slots[1] = (grom_match){22, "beta"};
    }
    out->administration = mode == MULTIPLE || mode == ADMIN_ONLY;
    return GROM_BACKEND_OK;
}

grom_backend_result grom_backend_select_slot(grom_auth_session session,
                                             grom_slot_choice choice) {
    assert(session == 7 && choice == 11);
    selections++;
    return GROM_BACKEND_UNAVAILABLE;
}

grom_backend_result grom_backend_begin_provisioning(grom_auth_session session,
                                                    grom_provision_session *out) {
    assert(session == 7);
    administration++;
    *out = 3;
    return GROM_BACKEND_OK;
}

void grom_backend_cancel_auth(grom_auth_session session) {
    assert(session == 7);
    auth_cancels++;
}

grom_backend_result grom_backend_preserve(grom_provision_session session,
                                          const uint8_t *secret, size_t length) {
    (void)session; (void)secret; (void)length;
    return GROM_BACKEND_UNAVAILABLE;
}

grom_backend_result grom_backend_preservation(grom_provision_session session,
                                              grom_preservation *out) {
    (void)session; (void)out;
    return GROM_BACKEND_UNAVAILABLE;
}

grom_backend_result grom_backend_prepare_new_slot(grom_provision_session session,
                                                  const char *label, uint64_t size,
                                                  const uint8_t *secret,
                                                  size_t length) {
    (void)session; (void)label; (void)size; (void)secret; (void)length;
    return GROM_BACKEND_UNAVAILABLE;
}

void grom_backend_discard_new_slot(grom_provision_session session) {
    (void)session;
}

grom_backend_result grom_backend_create_slot(grom_provision_session session,
                                             bool acknowledged) {
    (void)session; (void)acknowledged;
    return GROM_BACKEND_UNAVAILABLE;
}

void grom_backend_cancel_provisioning(grom_provision_session session) {
    assert(session == 3);
    provision_cancels++;
}

grom_backend_result grom_backend_charge_status(grom_charge_status *out) {
    charge_queries++;
    *out = (grom_charge_status){.percent_known = true, .percent = 42};
    return GROM_BACKEND_OK;
}

grom_backend_result grom_backend_power(grom_power_action action) {
    (void)action;
    return GROM_BACKEND_UNAVAILABLE;
}

static void flush(lv_display_t *display, const lv_area_t *area,
                  uint8_t *pixels) {
    (void)area; (void)pixels;
    lv_display_flush_ready(display);
}

static lv_obj_t *content(lv_display_t *display) {
    lv_obj_t *screen = lv_display_get_screen_active(display);
    assert(lv_obj_get_child_count(screen) >= 1);
    return lv_obj_get_child(screen, 0);
}

static void enter_and_unlock(lv_display_t *display) {
    lv_obj_t *root = content(display);
    assert(lv_obj_get_child_count(root) == 3);
    lv_obj_t *screen = lv_display_get_screen_active(display);
    assert(lv_obj_get_child_count(screen) == 2);
    lv_obj_t *keyboard = lv_obj_get_child(screen, 1);
    uint32_t key_index = 0;
    const char *key = NULL;
    for(; key_index < 50; key_index++) {
        key = lv_buttonmatrix_get_button_text(keyboard, key_index);
        if(key && strlen(key) == 1 && isalnum((unsigned char)key[0])) break;
    }
    assert(key_index < 50);
    lv_buttonmatrix_set_selected_button(keyboard, key_index);
    lv_obj_send_event(keyboard, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *unlock = lv_obj_get_child(root, 2);
    assert(!lv_obj_has_state(unlock, LV_STATE_DISABLED));
    lv_obj_send_event(unlock, LV_EVENT_CLICKED, NULL);
}

static unsigned labels_equal(lv_obj_t *object, const char *wanted) {
    unsigned count = 0;
    if(lv_obj_check_type(object, &lv_label_class) &&
       strcmp(lv_label_get_text(object), wanted) == 0) count++;
    uint32_t children = lv_obj_get_child_count(object);
    for(uint32_t i = 0; i < children; i++)
        count += labels_equal(lv_obj_get_child(object, i), wanted);
    return count;
}

int main(void) {
    static uint8_t pixels[480 * 32 * 4];
    lv_init();
    lv_display_t *display = lv_display_create(480, 900);
    assert(display);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    grom_ui_frontend *ui = grom_ui_create(display);
    assert(ui);

    mode = NO_MATCH;
    assert(grom_ui_show_password(ui) == 0);
    enter_and_unlock(display);
    assert(authentications == 1 && selections == 0 && auth_cancels == 0);
    lv_timer_handler();
    assert(labels_equal(content(display),
                        "Unable to unlock. Try again.") == 1);

    mode = MULTIPLE;
    assert(grom_ui_show_password(ui) == 0);
    enter_and_unlock(display);
    assert(authentications == 2 && selections == 0 && administration == 0);
    lv_timer_handler();
    lv_obj_t *root = content(display);
    assert(labels_equal(root, "alpha") == 1);
    assert(labels_equal(root, "beta") == 1);
    assert(labels_equal(root, "Administration") == 1);
    assert(grom_ui_show_charging(ui) == 0);
    assert(auth_cancels == 1 && charge_queries == 1);

    mode = SINGLE;
    assert(grom_ui_show_password(ui) == 0);
    enter_and_unlock(display);
    assert(authentications == 3 && selections == 1 && auth_cancels == 2);

    mode = ADMIN_ONLY;
    assert(grom_ui_show_password(ui) == 0);
    enter_and_unlock(display);
    assert(authentications == 4 && administration == 1);
    assert(grom_ui_show_password(ui) == 0);
    assert(provision_cancels == 1);

    mode = INTERNAL_ERROR;
    enter_and_unlock(display);
    assert(authentications == 5 && selections == 1 && administration == 1);
    lv_timer_handler();
    assert(labels_equal(content(display),
                        "Preboot unavailable") == 1);

    assert(grom_ui_show_password(ui) == 0);
    lv_display_set_resolution(display, 1080, 2400);
    root = content(display);
    lv_obj_update_layout(root);
    assert(lv_obj_get_style_transform_scale_x(root, 0) == 576);
    assert(lv_obj_get_style_transform_scale_y(root, 0) == 576);
    assert(lv_obj_get_x(root) == 0);
    assert(lv_obj_get_y(root) == 187);
    lv_obj_t *keyboard = lv_obj_get_child(lv_display_get_screen_active(display), 1);
    lv_area_t keyboard_bounds;
    lv_obj_get_coords(keyboard, &keyboard_bounds);
    assert(keyboard_bounds.x1 == 0 && keyboard_bounds.y1 == 1528);
    assert(lv_area_get_width(&keyboard_bounds) == 1080);
    assert(lv_area_get_height(&keyboard_bounds) == 684);
    lv_obj_t *field = lv_obj_get_child(root, 1);
    lv_obj_t *unlock = lv_obj_get_child(root, 2);
    lv_area_t bounds;
    lv_obj_get_coords(field, &bounds);
    lv_obj_get_transformed_area(field, &bounds,
                                LV_OBJ_POINT_TRANSFORM_FLAG_RECURSIVE);
    assert(bounds.x1 >= 230 && bounds.x2 <= 855);
    assert(bounds.y1 >= 1135 && bounds.y2 <= 1300);
    lv_point_t point = {(bounds.x1 + bounds.x2) / 2,
                        (bounds.y1 + bounds.y2) / 2};
    assert(lv_indev_search_obj(lv_display_get_screen_active(display),
                               &point) == field);
    lv_obj_get_coords(unlock, &bounds);
    lv_obj_get_transformed_area(unlock, &bounds,
                                LV_OBJ_POINT_TRANSFORM_FLAG_RECURSIVE);
    point = (lv_point_t){(bounds.x1 + bounds.x2) / 2,
                         (bounds.y1 + bounds.y2) / 2};
    assert(lv_indev_search_obj(lv_display_get_screen_active(display),
                               &point) == unlock);

    lv_display_set_resolution(display, 800, 600);
    lv_obj_update_layout(root);
    assert(lv_obj_get_style_transform_scale_x(root, 0) == 170);
    assert(lv_obj_get_style_transform_scale_y(root, 0) == 170);
    assert(lv_obj_get_x(root) >= 240 && lv_obj_get_x(root) <= 241);
    assert(lv_obj_get_y(root) == 1);
    grom_ui_destroy(ui);
    lv_display_delete(display);
    lv_deinit();
}
