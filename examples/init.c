/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

#include "miniui.h"
#include "timing.h"

#ifdef MUI_USE_LVGL
int run_lvgl(mui_display *display, int hold);
#endif

/* Emulator harness only. A production init owns its own boot and device policy. */
static void load_emulator_modules(void) {
    const char *modules[] = {"virtio_pci_modern_dev", "virtio_pci", "virtio_dma_buf", "virtio-gpu"};

    for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]); ++i) {
        char path[128];
        snprintf(path, sizeof(path), "/lib/modules/%s.ko", modules[i]);

        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        if (syscall(SYS_finit_module, fd, "", 0) < 0) {
            perror(path);
        }
        close(fd);
    }
}

static void early_boot(void) {
    mkdir("/proc", 0755);
    mkdir("/sys", 0755);
    mkdir("/dev", 0755);
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    mkdir("/dev/dri", 0755);
    mkdir("/dev/graphics", 0755);
    load_emulator_modules();
}

static void create_node(const char *sys_path, const char *dev_path) {
    FILE *file = fopen(sys_path, "r");
    if (!file) {
        return;
    }

    unsigned major, minor;
    if (fscanf(file, "%u:%u", &major, &minor) == 2) {
        mknod(dev_path, S_IFCHR | 0600, makedev(major, minor));
    }
    fclose(file);
}

static void create_display_nodes(void) {
    for (unsigned i = 0; i < 64; ++i) {
        char sys_path[128];
        char dev_path[64];

        snprintf(sys_path, sizeof(sys_path), "/sys/class/drm/card%u/dev", i);
        snprintf(dev_path, sizeof(dev_path), "/dev/dri/card%u", i);
        create_node(sys_path, dev_path);

        snprintf(sys_path, sizeof(sys_path), "/sys/class/graphics/fb%u/dev", i);
        snprintf(dev_path, sizeof(dev_path), "/dev/fb%u", i);
        create_node(sys_path, dev_path);
    }
}

static void hide_drm_nodes(void) {
    /* Test-only: force automatic discovery to reach a real fbdev device. */
    for (unsigned i = 0; i < 64; ++i) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/dri/card%u", i);
        unlink(path);
    }
}

static void set_graphics_console(void) {
    int tty = open("/dev/tty0", O_RDWR | O_CLOEXEC);
    if (tty >= 0) {
        ioctl(tty, KDSETMODE, KD_GRAPHICS);
        close(tty);
    }
}

static void supervise_renderer(pid_t child) {
    if (child < 0) {
        perror("fork renderer");
    } else {
        int status;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
        fprintf(stderr, "renderer stopped; init remains alive\n");
    }

    for (;;) {
        while (waitpid(-1, NULL, WNOHANG) > 0) {
        }
        sleep(1);
    }
}

static mui_backend read_backend(char *mode, size_t size, int argc, char **argv) {
    snprintf(mode, size, "auto");
    FILE *file = fopen("/miniui-backend", "r");
    if (file) {
        if (!fgets(mode, size, file)) {
            mode[0] = '\0';
        }
        fclose(file);
    }
    if (argc > 1) {
        snprintf(mode, size, "%s", argv[1]);
    }

    if (strncmp(mode, "fbdev", 5) == 0) {
        return MUI_FBDEV;
    }
    if (strncmp(mode, "drm", 3) == 0) {
        return MUI_DRM;
    }
    return MUI_AUTO;
}

static mui_display *open_display(mui_backend backend, const char *mode, int init_mode) {
    mui_options options = {.backend = backend};
    mui_display *display = NULL;

    for (int attempt = 0; attempt < 50; ++attempt) {
        if (init_mode) {
            create_display_nodes();
        }
        if (strncmp(mode, "auto-fbdev", 10) == 0) {
            hide_drm_nodes();
        }
        if (mui_open(&display, &options) == 0) {
            return display;
        }
        usleep(100000);
    }
    return NULL;
}

#ifndef MUI_USE_LVGL
static void draw_pattern(const mui_surface *surface, unsigned frame) {
    static const uint32_t colors[] = {0xff0000, 0x00ff00, 0x0000ff, 0xffffff,
                                      0x00ffff, 0xff00ff, 0xffff00, 0x000000};

    for (uint32_t y = 0; y < surface->height; ++y) {
        for (uint32_t x = 0; x < surface->width; ++x) {
            uint32_t color = colors[(uint64_t)x * 8 / surface->width];
            if (y >= surface->height / 2) {
                color = ((x / 32 + y / 32) % 2) ? 0xdddddd : 0x222222;
            }
            if (x < 8 || y < 8 || x >= surface->width - 8 || y >= surface->height - 8) {
                color = 0xffffff;
            }
            if (y > surface->height * 3 / 4 && y < surface->height * 3 / 4 + 32 &&
                x < frame * surface->width / 120) {
                color = 0xff8000;
            }
            surface->pixels[(size_t)y * surface->width + x] = color;
        }
    }
}

static int run_pattern(mui_display *display, int hold) {
    const mui_surface *surface = mui_buffer(display);
    struct frame_timings timings = {0};
    for (unsigned frame = 0; frame < 120; ++frame) {
        draw_pattern(surface, frame);
        uint64_t started = monotonic_ns();
        if (mui_present(display) < 0) {
            perror("present");
            return 1;
        }
        record_frame(&timings, monotonic_ns() - started);
        usleep(16000);
    }
    report_frames("present", &timings);
    if (hold) {
        for (;;) {
            pause();
        }
    }
    sleep(2);
    return 0;
}
#endif

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    int init_mode = getpid() == 1;
    if (init_mode) {
        early_boot();
        pid_t child = fork();
        if (child != 0) {
            supervise_renderer(child);
        }
    }

    fprintf(stderr, "early boot complete\n");
    set_graphics_console();

    char mode[32];
    mui_backend backend = read_backend(mode, sizeof(mode), argc, argv);
    mui_display *display = open_display(backend, mode, init_mode);
    if (!display) {
        perror("miniui open");
        return 1;
    }

    const mui_surface *surface = mui_buffer(display);
    printf("miniui backend=%d %ux%u\n", mui_backend_used(display), surface->width, surface->height);

#ifdef MUI_USE_LVGL
    int result = run_lvgl(display, init_mode);
#else
    int result = run_pattern(display, init_mode);
#endif
    if (mui_close(display) < 0) {
        perror("miniui close");
        result = 1;
    }
    return result;
}
