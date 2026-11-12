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
#include "grom_ui_frontend.h"
#include "debug_ui.h"
#include "touch.h"
#include "power_key.h"
#include "power_request.h"
#include "grom_backend.h"

static int mounted_devtmpfs;
static unsigned diagnostic_events;
static uint32_t last_touch_age_ms;
static uint32_t max_touch_age_ms;

typedef struct {
    grom_touch *touch;
    grom_debug_ui *debug;
} input_context;

static int diagnostic_number(const char *text) {
    int value = 0;
    while (*text == ' ' || *text == '\t') ++text;
    while (*text >= '0' && *text <= '9') value = value * 10 + *text++ - '0';
    return value;
}

static const char *diagnostic_find(const char *text, const char *needle) {
    size_t length = strlen(needle);
    for (; *text; ++text) {
        size_t remaining = strlen(text);
        if (remaining < length) break;
        if (memcmp(text, needle, length) == 0) return text;
    }
    return NULL;
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

static void create_input_nodes(void) {
    mkdir("/dev/input", 0755);
    create_class_nodes("/sys/class/input", "event", "/dev/input/event%u", 128);
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
    mkdir("/dev/input", 0755);
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

static void read_touch(lv_indev_t *indev, lv_indev_data_t *data) {
    input_context *context = lv_indev_get_user_data(indev);
    grom_touch *touch = context->touch;
    if(context->debug->injecting) {
        data->point.x = context->debug->x;
        data->point.y = context->debug->y;
        data->state = context->debug->pressed ? LV_INDEV_STATE_PRESSED :
                                                LV_INDEV_STATE_RELEASED;
        return;
    }
    grom_touch_point point;
    int result = grom_touch_read(touch, &point);
    if(result == 1) {
        ++diagnostic_events;
        uint32_t age = tick_ms() - touch->last_event_ms;
        if(age < 60000) {
            last_touch_age_ms = age;
            if(age > max_touch_age_ms) max_touch_age_ms = age;
        }
    }
    if(touch->cancel_pending) {
        lv_indev_reset(indev, NULL);
        touch->cancel_pending = false;
    }
    if(result == 1) data->continue_reading = true;
    data->point.x = touch->point.x;
    data->point.y = touch->point.y;
    data->state = touch->point.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static int module_loaded(const char *name) {
    int fd = open("/proc/modules", O_RDONLY | O_CLOEXEC);
    if(fd < 0) return 0;
    char contents[16384];
    ssize_t count = read(fd, contents, sizeof(contents));
    close(fd);
    if(count <= 0) return 0;
    size_t length = strlen(name);
    for(size_t start = 0; start < (size_t)count;) {
        size_t end = start;
        while(end < (size_t)count && contents[end] != '\n') ++end;
        if(end - start > length && memcmp(contents + start, name, length) == 0 &&
           contents[start + length] == ' ') return 1;
        start = end + 1;
    }
    return 0;
}

static void diagnostic_modules(char *output, size_t capacity) {
    int fd = open("/grom/touch-diagnostics", O_RDONLY | O_CLOEXEC);
    if(fd < 0) return;
    char contents[1024];
    ssize_t count = read(fd, contents, sizeof(contents));
    close(fd);
    if(count <= 0) return;
    size_t used = 0;
    char name[160];
    for(size_t start = 0; start < (size_t)count && used + 32 < capacity;) {
        size_t end = start;
        while(end < (size_t)count && contents[end] != '\n') ++end;
        size_t length = end - start;
        if(length >= sizeof(name)) break;
        memcpy(name, contents + start, length);
        name[length] = '\0';
        start = end + 1;
        if(length < 4 || strcmp(name + length - 3, ".ko") != 0) continue;
        name[length - 3] = '\0';
        for(char *p = name; *p; ++p) if(*p == '-') *p = '_';
        int added = snprintf(output + used, capacity - used, "%s: %s\n",
                             name, module_loaded(name) ? "loaded" : "MISSING");
        if(added < 0 || (size_t)added >= capacity - used) break;
        used += (size_t)added;
    }
}

static void diagnostic_last_kernel_line(char *output, size_t capacity) {
    static char log[65536];
    long count = syscall(SYS_syslog, 3, log, sizeof(log));
    if(count <= 0) {
        snprintf(output, capacity, "kernel log unavailable");
        return;
    }
    size_t end = (size_t)count;
    while(end) {
        size_t start = end;
        while(start && log[start - 1] != '\n') --start;
        size_t length = end - start;
        if(length && (memmem(log + start, length, "HXTP", 4) ||
                      memmem(log + start, length, "firmware", 8) ||
                      memmem(log + start, length, "grom: driver", 12) ||
                      memmem(log + start, length, "adbd", 4) ||
                      memmem(log + start, length, "linker", 6))) {
            size_t copied = length < capacity - 1 ? length : capacity - 1;
            for(size_t i = 0; i < copied; ++i) {
                unsigned char c = (unsigned char)log[start + i];
                output[i] = (c >= 32 && c <= 126) ? (char)c : ' ';
            }
            output[copied] = '\0';
            return;
        }
        end = start ? start - 1 : 0;
    }
    snprintf(output, capacity, "no touch kernel messages");
}

static void update_touch_diagnostics(lv_obj_t *label, const grom_touch *touch) {
    char modules[320] = {0};
    char class_name[64] = "none";
    char kernel[120];
    diagnostic_modules(modules, sizeof(modules));
    DIR *dir = opendir("/sys/class/touchscreen");
    if(dir) {
        struct dirent *entry;
        while((entry = readdir(dir))) {
            if(entry->d_name[0] != '.') {
                snprintf(class_name, sizeof(class_name), "%s", entry->d_name);
                break;
            }
        }
        closedir(dir);
    }
    diagnostic_last_kernel_line(kernel, sizeof(kernel));
    char usb[160] = "USB ADB: status unavailable";
    char adb_log[700] = "adbd log: empty";
    char adb_children[180] = "adbd children: none";
    int adbd_pid = -1;
    DIR *processes = opendir("/proc");
    if (processes) {
        struct dirent *entry;
        while ((entry = readdir(processes))) {
            if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
            char path[80], comm[32] = {0};
            snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);
            int fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            ssize_t n = read(fd, comm, sizeof(comm) - 1);
            close(fd);
            if (n >= 5 && memcmp(comm, "adbd\n", 5) == 0) {
                adbd_pid = diagnostic_number(entry->d_name);
                break;
            }
        }
        closedir(processes);
    }
    if (adbd_pid > 0) {
        processes = opendir("/proc");
        if (processes) {
            struct dirent *entry;
            while ((entry = readdir(processes))) {
                if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
                char path[80], status[512], wchan[48] = {0};
                snprintf(path, sizeof(path), "/proc/%s/status", entry->d_name);
                int fd = open(path, O_RDONLY | O_CLOEXEC);
                if (fd < 0) continue;
                ssize_t n = read(fd, status, sizeof(status) - 1);
                close(fd);
                if (n <= 0) continue;
                status[n] = 0;
                const char *parent = diagnostic_find(status, "\nPPid:");
                if (!parent || diagnostic_number(parent + 6) != adbd_pid) continue;
                snprintf(path, sizeof(path), "/proc/%s/wchan", entry->d_name);
                fd = open(path, O_RDONLY | O_CLOEXEC);
                if (fd >= 0) {
                    n = read(fd, wchan, sizeof(wchan) - 1);
                    close(fd);
                    if (n > 0) wchan[n] = 0;
                }
                const char *state = diagnostic_find(status, "\nState:");
                snprintf(adb_children, sizeof(adb_children),
                         "adbd child %s: %c %s", entry->d_name,
                         state ? state[8] : '?', wchan);
                break;
            }
            closedir(processes);
        }
    }
    int usb_fd = open("/grom/adb-status", O_RDONLY | O_CLOEXEC);
    if (usb_fd >= 0) {
        ssize_t n = read(usb_fd, usb, sizeof(usb) - 1);
        close(usb_fd);
        if (n > 0) {
            usb[n] = 0;
            if (usb[n - 1] == '\n') usb[n - 1] = 0;
        }
    }
    char log_path[160] = "/grom/adbd.log";
    DIR *logs = opendir("/data/adb");
    if (logs) {
        char newest[128] = {0};
        struct dirent *entry;
        while ((entry = readdir(logs))) {
            if (entry->d_name[0] == 'a' && entry->d_name[1] == 'd' &&
                entry->d_name[2] == 'b' && entry->d_name[3] == '-' &&
                strcmp(entry->d_name, newest) > 0)
                snprintf(newest, sizeof(newest), "%s", entry->d_name);
        }
        closedir(logs);
        if (newest[0]) snprintf(log_path, sizeof(log_path), "/data/adb/%s", newest);
    }
    int log_fd = open(log_path, O_RDONLY | O_CLOEXEC);
    if (log_fd >= 0) {
        char contents[4096];
        off_t size = lseek(log_fd, 0, SEEK_END);
        lseek(log_fd, size > (off_t)sizeof(contents) ? size - (off_t)sizeof(contents) : 0,
              SEEK_SET);
        ssize_t n = read(log_fd, contents, sizeof(contents));
        close(log_fd);
        if (n > 0) {
            size_t end = (size_t)n;
            while (end && (contents[end - 1] == '\n' || contents[end - 1] == '\r')) --end;
            size_t start = end;
            unsigned lines = 0;
            while (start) {
                --start;
                if (contents[start] == '\n' && ++lines == 6) {
                    ++start;
                    break;
                }
            }
            size_t length = end - start;
            if (length >= sizeof(adb_log)) length = sizeof(adb_log) - 1;
            memcpy(adb_log, contents + start, length);
            adb_log[length] = 0;
        }
    }
    char status[2200];
    snprintf(status, sizeof(status),
             "TOUCH DIAGNOSTICS\n%sclass: %s\ninput fd: %d  protocol: %d  blocked: %d\nevents: %u  pressed: %d  x: %d  y: %d\n%s\n%s\n%s\n%s",
             modules, class_name, touch->fd, touch->protocol, touch->blocked,
             diagnostic_events, touch->point.pressed, touch->point.x, touch->point.y,
             kernel, usb, adb_children, adb_log);
    lv_label_set_text(label, status);
}

int grom_test_ui_run(int ready_fd, int power_fd) {
    grom_backend_set_power_fd(power_fd);
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
    const gromui_surface *surface = gromui_buffer(backend);
    grom_debug_ui debug;
    grom_debug_ui_start(&debug);
    grom_touch touch;
    grom_touch_init(&touch, surface->width, surface->height);
    if(!mounted_devtmpfs) create_input_nodes();
    (void)grom_touch_discover(&touch);
    lv_indev_t *indev = lv_indev_create();
    if(!indev) {
        grom_debug_ui_stop(&debug);
        grom_touch_close(&touch);
        lv_display_delete(display);
        lv_deinit();
        gromui_close(backend);
        return 1;
    }
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(indev, display);
    lv_indev_set_read_cb(indev, read_touch);
    input_context input = {.touch = &touch, .debug = &debug};
    lv_indev_set_user_data(indev, &input);
    /* The renderer owns the evdev queue. Poll on every loop so input does not
     * depend on LVGL's refresh timer schedule while the preboot UI is idle. */
    lv_timer_pause(lv_indev_get_read_timer(indev));
    grom_ui_frontend *frontend = grom_ui_create(display);
    if (!frontend || grom_ui_show_password(frontend) != 0) {
        if (frontend) grom_ui_destroy(frontend);
        lv_indev_delete(indev);
        grom_debug_ui_stop(&debug);
        grom_touch_close(&touch);
        lv_display_delete(display);
        lv_deinit();
        gromui_close(backend);
        return 1;
    }
    grom_power_key power_key;
    grom_power_key_init(&power_key);
    grom_power_key_scan(&power_key);
    lv_obj_t *diagnostics = NULL;
    if(access("/grom/touch-diagnostics", R_OK) == 0) {
        diagnostics = lv_label_create(lv_screen_active());
        lv_obj_set_width(diagnostics, surface->width > 32 ? surface->width - 32 : surface->width);
        lv_label_set_long_mode(diagnostics, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_bg_color(diagnostics, lv_color_hex(0x101010), 0);
        lv_obj_set_style_bg_opa(diagnostics, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(diagnostics, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_pad_all(diagnostics, 10, 0);
        lv_obj_remove_flag(diagnostics, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(diagnostics, LV_ALIGN_TOP_MID, 0, 8);
        update_touch_diagnostics(diagnostics, &touch);
    }
    lv_refr_now(display);
    if (gromui_lvgl_error(display) || gromui_lvgl_presents(display) == 0) {
        grom_ui_destroy(frontend);
        lv_indev_delete(indev);
        grom_debug_ui_stop(&debug);
        grom_touch_close(&touch);
        lv_display_delete(display);
        lv_deinit();
        gromui_close(backend);
        return 1;
    }
    (void)write(ready_fd, "1", 1);
    close(ready_fd);

    uint32_t last_probe = tick_ms();
    uint32_t last_diagnostic = last_probe;
    uint32_t last_timing = last_probe;
    uint32_t max_handler_ms = 0;
    uint32_t last_handler_ms = 0;
    for (;;) {
        grom_debug_ui_poll(&debug, indev, display, backend);
        if(grom_power_key_poll(&power_key) &&
           grom_backend_power(GROM_POWER_SHUTDOWN) != GROM_BACKEND_OK)
            fprintf(stderr, "grom: power key shutdown request failed\n");
        lv_indev_read(indev);
        if((uint32_t)(tick_ms() - last_probe) >= 1000) {
            last_probe = tick_ms();
            if(!mounted_devtmpfs) create_input_nodes();
            if(touch.fd < 0) (void)grom_touch_discover(&touch);
            grom_power_key_scan(&power_key);
        }
        if(diagnostics && (uint32_t)(tick_ms() - last_diagnostic) >= 1000) {
            last_diagnostic = tick_ms();
            update_touch_diagnostics(diagnostics, &touch);
        }
        uint32_t handler_start = tick_ms();
        lv_timer_handler();
        last_handler_ms = tick_ms() - handler_start;
        if(last_handler_ms > max_handler_ms) max_handler_ms = last_handler_ms;
        if(access("/grom/debug-adb", R_OK) == 0 &&
           (uint32_t)(tick_ms() - last_timing) >= 1000) {
            char timing[256];
            int n = snprintf(timing, sizeof(timing),
                             "events=%u fd=%d protocol=%d blocked=%d dropped=%u pressed=%d x=%d y=%d touch_age_ms=%u max_touch_age_ms=%u handler_ms=%u max_handler_ms=%u\n",
                             diagnostic_events, touch.fd, touch.protocol, touch.blocked,
                             touch.dropped_frames, touch.point.pressed,
                             touch.point.x, touch.point.y,
                             last_touch_age_ms, max_touch_age_ms,
                             last_handler_ms, max_handler_ms);
            int fd = open("/grom/ui-timing", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if(fd >= 0) { write(fd, timing, (size_t)n); close(fd); }
            last_timing = tick_ms();
        }
        if (gromui_lvgl_error(display)) {
            break;
        }
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 10000000};
        nanosleep(&delay, NULL);
    }
    grom_ui_destroy(frontend);
    lv_indev_delete(indev);
    grom_debug_ui_stop(&debug);
    grom_power_key_close(&power_key);
    grom_touch_close(&touch);
    lv_display_delete(display);
    lv_deinit();
    gromui_close(backend);
    return 1;
}
