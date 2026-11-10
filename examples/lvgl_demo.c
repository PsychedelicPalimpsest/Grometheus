/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "miniui_lvgl.h"
#include "timing.h"
static uint32_t tick(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000);
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, const lv_font_t *font,
                       uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

int run_lvgl(mui_display *backend, int hold) {
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *display = mui_lvgl_create(backend);
    if (!display) {
        perror("LVGL display");
        lv_deinit();
        return 1;
    }
    lv_obj_t *screen = lv_display_get_screen_active(display);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), 0);
    label(screen, "MINIUI / RECOVERY", 36, 30, &lv_font_montserrat_24, 0x69dfb0);
    label(screen, "Display is ready", 36, 90, &lv_font_montserrat_32, 0xffffff);
    label(screen,
          mui_backend_used(backend) == MUI_DRM ? "LVGL 9.4  /  DRM framebuffer"
                                               : "LVGL 9.4  /  fbdev framebuffer",
          36, 142, &lv_font_montserrat_24, 0xa7b8c8);
    const uint32_t colors[] = {0xff0000, 0x00ff00, 0x0000ff, 0xffffff, 0x00ffff, 0xff00ff};
    for (int i = 0; i < 6; i++) {
        lv_obj_t *box = lv_obj_create(screen);
        lv_obj_set_pos(box, 36 + i * 120, 205);
        lv_obj_set_size(box, 108, 70);
        lv_obj_set_style_bg_color(box, lv_color_hex(colors[i]), 0);
        lv_obj_set_style_border_width(box, 0, 0);
    }
    label(screen, "Partial updates preserve the rest of the frame", 36, 305, &lv_font_montserrat_24,
          0xffffff);
    lv_obj_t *bar = lv_bar_create(screen);
    lv_obj_set_pos(bar, 36, 355);
    lv_obj_set_size(bar, 728, 24);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x69dfb0), LV_PART_INDICATOR);
    lv_obj_t *status = label(screen, "Rendering...", 36, 406, &lv_font_montserrat_24, 0xa7b8c8);
    lv_obj_t *button = lv_button_create(screen);
    lv_obj_set_pos(button, 36, 474);
    lv_obj_set_size(button, 270, 64);
    label(button, "Recovery display", 12, 4, &lv_font_montserrat_24, 0xffffff);
    uint32_t start = tick();
    unsigned old = 101;
    struct frame_timings timings = {0};
    int reported = 0;
    for (;;) {
        unsigned progress = (tick() - start) / 20;
        if (progress > 100) {
            progress = 100;
        }
        if (progress != old) {
            lv_bar_set_value(bar, (int32_t)progress, LV_ANIM_OFF);
            old = progress;
            if (progress == 100) {
                lv_label_set_text(status, "Color, text and partial updates complete");
            }
        }
        uint64_t started = monotonic_ns();
        lv_timer_handler();
        if (!reported) {
            record_frame(&timings, monotonic_ns() - started);
            if (progress == 100) {
                report_frames("lvgl-handler", &timings);
                reported = 1;
            }
        }
        if (mui_lvgl_error(display)) {
            fprintf(stderr, "LVGL present errno=%d\n", mui_lvgl_error(display));
            break;
        }
        if (!hold && tick() - start > 4000) {
            break;
        }
        struct timespec delay = {0, 10000000};
        nanosleep(&delay, NULL);
    }
    int result = mui_lvgl_error(display) ? 1 : 0;
    lv_display_delete(display);
    lv_deinit();
    return result;
}
