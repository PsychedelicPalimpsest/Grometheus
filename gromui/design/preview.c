/* Native host entry point. The UI itself lives in grom/ui/outline.c. */
#include "grom_ui_frontend.h"
#include "lvgl.h"
#include <SDL.h>

int main(void) {
    lv_init();
    lv_tick_set_cb(SDL_GetTicks);
    lv_display_t *display = lv_sdl_window_create(480, 900);
    if(!display) {
        lv_deinit();
        return 1;
    }
    lv_sdl_window_set_title(display, "grom");
    lv_sdl_mouse_create();

    grom_ui_frontend *frontend = grom_ui_create(display);
    if(!frontend || grom_ui_show_password(frontend) != 0) {
        if(frontend) grom_ui_destroy(frontend);
        lv_display_delete(display);
        lv_deinit();
        return 1;
    }
    for(;;) {
        lv_timer_handler();
        SDL_Delay(5);
    }
}
