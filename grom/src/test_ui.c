#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "test_ui.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include "gromui_lvgl.h"

static int mounted_devtmpfs;

static void load_emulator_modules(void) {
    if (access("/lib/modules/virtio-gpu.ko", R_OK) != 0) {
        return;
    }
    static const char *modules[] = {
        "virtio_pci_modern_dev", "virtio_pci", "virtio_dma_buf", "virtio-gpu"
    };
    for (unsigned i = 0; i < sizeof(modules) / sizeof(modules[0]); ++i) {
        char path[96];
        snprintf(path, sizeof(path), "/lib/modules/%s.ko", modules[i]);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            syscall(SYS_finit_module, fd, "", 0);
            close(fd);
        }
    }
}

static int parse_device(const char *text, unsigned *major, unsigned *minor) {
    unsigned values[2] = {0, 0};
    unsigned part = 0;
    for (const char *p = text; *p && *p != '\n'; ++p) {
        if (*p == ':' && part == 0) {
            part = 1;
            continue;
        }
        if (*p < '0' || *p > '9' || values[part] > 65535) {
            return -1;
        }
        values[part] = values[part] * 10 + (unsigned)(*p - '0');
    }
    if (part != 1 || values[0] > 65535 || values[1] > 65535) {
        return -1;
    }
    *major = values[0];
    *minor = values[1];
    return 0;
}

static void create_node(const char *sys_path, const char *dev_path) {
    char text[32] = {0};
    int fd = open(sys_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    ssize_t count = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (count <= 0) {
        return;
    }
    unsigned major, minor;
    if (parse_device(text, &major, &minor) == 0) {
        mknod(dev_path, S_IFCHR | 0600, makedev(major, minor));
    }
}

static void create_class_nodes(const char *directory, const char *prefix,
                               const char *destination, unsigned limit) {
    DIR *dir = opendir(directory);
    if (!dir) {
        return;
    }
    size_t prefix_length = strlen(prefix);
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        const char *name = entry->d_name;
        if (memcmp(name, prefix, prefix_length) != 0) {
            continue;
        }
        const char *digits = name + prefix_length;
        if (*digits < '0' || *digits > '9') {
            continue;
        }
        unsigned number = 0;
        for (; *digits >= '0' && *digits <= '9'; ++digits) {
            number = number * 10 + (unsigned)(*digits - '0');
            if (number >= limit) {
                break;
            }
        }
        if (*digits || number >= limit) {
            continue;
        }
        char sys_path[128], dev_path[64];
        snprintf(sys_path, sizeof(sys_path), "%s/%s/dev", directory, name);
        snprintf(dev_path, sizeof(dev_path), destination, number);
        create_node(sys_path, dev_path);
    }
    closedir(dir);
}

static void create_display_nodes(void) {
    create_class_nodes("/sys/class/drm", "card", "/dev/dri/card%u", 64);
    create_class_nodes("/sys/class/graphics", "fb", "/dev/fb%u", 32);
}

static int prepare_devices(void) {
    /* Keep renderer mounts private so they cannot disturb MagiskInit's later
     * mount ownership when a real preboot handoff is implemented. */
    if (syscall(SYS_unshare, CLONE_NEWNS) < 0 ||
        mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        return -1;
    }
    mkdir("/dev", 0755);
    if (mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) == 0) {
        mounted_devtmpfs = 1;
    } else {
        if (mount("tmpfs", "/dev", "tmpfs", 0, "mode=755") < 0) {
            return -1;
        }
    }
    mkdir("/dev/dri", 0755);
    mkdir("/dev/graphics", 0755);
    load_emulator_modules();
    int tty = open("/dev/tty0", O_RDWR | O_CLOEXEC);
    if (tty >= 0) {
        ioctl(tty, KDSETMODE, KD_GRAPHICS);
        close(tty);
    }
    return 0;
}

static uint32_t tick_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)((uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000);
}

static lv_obj_t *label(lv_obj_t *parent, const char *value, int x, int y,
                       const lv_font_t *font, uint32_t color) {
    lv_obj_t *object = lv_label_create(parent);
    lv_label_set_text(object, value);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    return object;
}

static void draw_test_screen(lv_display_t *display) {
    lv_obj_t *screen = lv_display_get_screen_active(display);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *accent = lv_obj_create(screen);
    lv_obj_remove_style_all(accent);
    lv_obj_set_size(accent, lv_pct(100), 10);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(accent, lv_color_hex(0x69dfb0), 0);

    label(screen, "GROM", 64, 62, &lv_font_montserrat_32, 0xf5f7f8);
    lv_obj_t *card = lv_obj_create(screen);
    lv_obj_set_pos(card, 64, 165);
    lv_obj_set_size(card, lv_pct(84), 325);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x202d38), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    label(card, "PASSWORD", 30, 35, &lv_font_montserrat_32, 0xf5f7f8);

    lv_obj_t *field = lv_obj_create(card);
    lv_obj_remove_style_all(field);
    lv_obj_set_pos(field, 30, 105);
    lv_obj_set_size(field, lv_pct(90), 3);
    lv_obj_set_style_bg_opa(field, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(field, lv_color_hex(0x69dfb0), 0);

    label(card, "PREBOOT DISPLAY TEST", 30, 150, &lv_font_montserrat_24,
          0xa7b8c8);
    label(card, "INPUT NOT ACTIVE", 30, 205, &lv_font_montserrat_24,
          0xa7b8c8);
}

int grom_test_ui_run(int ready_fd) {
    if (prepare_devices() < 0) {
        return 1;
    }
    gromui_display *backend = NULL;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        /* Coldboot may publish devices after the first probe. Refresh once
         * per second instead of repeating all sysfs reads on every retry. */
        if (!mounted_devtmpfs && attempt % 10 == 0) {
            create_display_nodes();
        }
        if (gromui_open(&backend, NULL) == 0) {
            break;
        }
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000000};
        nanosleep(&delay, NULL);
    }
    if (!backend) {
        return 1;
    }

    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_display_t *display = gromui_lvgl_create(backend);
    if (!display) {
        lv_deinit();
        gromui_close(backend);
        return 1;
    }
    draw_test_screen(display);
    lv_refr_now(display);
    if (gromui_lvgl_error(display) || gromui_lvgl_presents(display) == 0) {
        lv_display_delete(display);
        lv_deinit();
        gromui_close(backend);
        return 1;
    }
    (void)write(ready_fd, "1", 1);
    close(ready_fd);

    for (;;) {
        lv_timer_handler();
        if (gromui_lvgl_error(display)) {
            break;
        }
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 50000000};
        nanosleep(&delay, NULL);
    }
    lv_display_delete(display);
    lv_deinit();
    gromui_close(backend);
    return 1;
}
