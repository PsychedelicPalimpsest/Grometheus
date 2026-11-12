/* SPDX-License-Identifier: Apache-2.0 */
#include "lvgl.h"

#include <assert.h>

static unsigned clicks;
static bool pressed, cancel;

static void clicked(lv_event_t *event) {
    (void)event;
    ++clicks;
}

static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data) {
    if(cancel) {
        lv_indev_reset(indev, NULL);
        cancel = false;
    }
    data->point.x = 50;
    data->point.y = 50;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

int main(void) {
    lv_init();
    lv_display_t *display = lv_display_create(101, 201);
    assert(display);
    lv_obj_t *button = lv_button_create(lv_screen_active());
    lv_obj_set_pos(button, 0, 0);
    lv_obj_set_size(button, 100, 100);
    lv_obj_add_event_cb(button, clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_update_layout(button);
    lv_indev_t *indev = lv_indev_create();
    assert(indev);
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(indev, display);
    lv_indev_set_read_cb(indev, read_pointer);

    pressed = true;
    lv_indev_read(indev);
    pressed = false;
    lv_indev_read(indev);
    assert(clicks == 1);

    pressed = true;
    lv_indev_read(indev);
    pressed = false;
    cancel = true;
    lv_indev_read(indev);
    assert(clicks == 1);

    lv_indev_delete(indev);
    lv_display_delete(display);
    lv_deinit();
    return 0;
}
