/* Native host entry point. The UI itself lives in grom/ui/outline.c. */
#include "grom_ui_frontend.h"
#include "lvgl.h"
#include <SDL.h>
#include <stdio.h>

int main(int argc, char **argv) {
    int width = 480, height = 900;
    char trailing;
    if(argc != 1 && (argc != 3 ||
       sscanf(argv[1], "%d%c", &width, &trailing) != 1 ||
       sscanf(argv[2], "%d%c", &height, &trailing) != 1 ||
       width < 1 || height < 1 || width > 4096 || height > 4096)) return 1;
    lv_init();
    lv_tick_set_cb(SDL_GetTicks);
    lv_display_t *display = lv_sdl_window_create(width, height);
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
