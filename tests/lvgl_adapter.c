#include <assert.h>
#include <errno.h>
#include <string.h>

#include "miniui_lvgl.h"
static uint32_t pixels[160 * 100];
static mui_surface surface = {160, 100, 160 * 4, pixels};
static int presentations, fail;
const mui_surface *mui_buffer(mui_display *d) {
    return d ? &surface : NULL;
}

int mui_present(mui_display *d) {
    (void)d;
    presentations++;
    if (fail) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int main(void) {
    lv_init();
    assert(!mui_lvgl_create(NULL));
    lv_display_t *display = mui_lvgl_create((mui_display *)1);
    assert(display);
    lv_obj_t *screen = lv_display_get_screen_active(display);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x123456), 0);
    lv_obj_t *box = lv_obj_create(screen);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, 20, 20);
    lv_obj_set_size(box, 20, 20);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(0xff0000), 0);
    lv_refr_now(display);
    assert(presentations == 1 && !mui_lvgl_error(display));
    assert((pixels[0] & 0xffffff) == 0x123456 && (pixels[25 * 160 + 25] & 0xffffff) == 0xff0000);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x00ff00), 0);
    lv_refr_now(display);
    assert(presentations == 2 && (pixels[0] & 0xffffff) == 0x123456 &&
           (pixels[25 * 160 + 25] & 0xffffff) == 0x00ff00);
    fail = 1;
    lv_obj_set_style_bg_color(box, lv_color_hex(0x0000ff), 0);
    lv_refr_now(display);
    assert(mui_lvgl_error(display) == EIO);
    lv_obj_invalidate(screen);
    lv_refr_now(display); /* Failure still completes the flush. */
    lv_display_delete(display);
    lv_deinit();
}
