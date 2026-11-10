/* Fault injection exercises actual public API and both kernel adapters. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "internal.h"
static int backend, fail_at, calls, opens, maps, handles, fbs, restored;
static unsigned char framebuffer[64];
int __wrap_open(const char *path, int flags, ...) {
    (void)flags;
    if (!strcmp(path, "/fake") ||
        (backend == MUI_FBDEV && (!strcmp(path, "/dev/fb0") || !strcmp(path, "/dev/dri/card0")))) {
        opens++;
        return 100;
    }
    errno = ENOENT;
    return -1;
}

int __wrap_close(int fd) {
    assert(fd == 100);
    opens--;
    return 0;
}

void *__wrap_mmap(void *addr, size_t size, int prot, int flags, int fd, off_t offset) {
    (void)addr;
    (void)prot;
    (void)flags;
    (void)offset;
    assert(fd == 100);
    if (++calls == fail_at) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    maps++;
    if (backend == MUI_FBDEV) {
        return framebuffer;
    }
    return calloc(1, size);
}

int __wrap_munmap(void *addr, size_t size) {
    (void)size;
    maps--;
    if (addr != framebuffer) {
        free(addr);
    }
    return 0;
}

int __wrap_ioctl(int fd, unsigned long req, ...) {
    assert(fd == 100);
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (req == DRM_IOCTL_MODE_RMFB) {
        fbs--;
        return 0;
    }
    if (req == DRM_IOCTL_MODE_DESTROY_DUMB) {
        handles--;
        return 0;
    }
    if (++calls == fail_at) {
        errno = EIO;
        return -1;
    }
    switch (req) {
        case FBIOGET_FSCREENINFO: {
            struct fb_fix_screeninfo *f = arg;
            *f = (struct fb_fix_screeninfo){.type = FB_TYPE_PACKED_PIXELS,
                                            .visual = FB_VISUAL_TRUECOLOR,
                                            .line_length = 16,
                                            .smem_len = 64};
            break;
        }
        case FBIOGET_VSCREENINFO: {
            struct fb_var_screeninfo *v = arg;
            *v = (struct fb_var_screeninfo){.xres = 4,
                                            .yres = 4,
                                            .xres_virtual = 4,
                                            .yres_virtual = 4,
                                            .bits_per_pixel = 32,
                                            .red = {16, 8, 0},
                                            .green = {8, 8, 0},
                                            .blue = {0, 8, 0}};
            break;
        }
        case FBIOBLANK:
            break;
        case DRM_IOCTL_SET_MASTER:
            break;
        case DRM_IOCTL_GET_CAP:
            ((struct drm_get_cap *)arg)->value = backend == MUI_DRM ? 1 : 0;
            break;
        case DRM_IOCTL_MODE_GETRESOURCES: {
            struct drm_mode_card_res *r = arg;
            *(uint32_t *)(uintptr_t)r->connector_id_ptr = 7;
            *(uint32_t *)(uintptr_t)r->crtc_id_ptr = 9;
            r->count_connectors = 1;
            r->count_crtcs = 1;
            break;
        }
        case DRM_IOCTL_MODE_GETCONNECTOR: {
            struct drm_mode_get_connector *c = arg;
            if (c->encoders_ptr) {
                *(uint32_t *)(uintptr_t)c->encoders_ptr = 8;
            }
            if (c->modes_ptr) {
                *(struct drm_mode_modeinfo *)(uintptr_t)c->modes_ptr =
                    (struct drm_mode_modeinfo){.hdisplay = 4, .vdisplay = 4};
            }
            c->connection = 1;
            c->count_modes = 1;
            c->count_encoders = 1;
            c->encoder_id = 8;
            break;
        }
        case DRM_IOCTL_MODE_GETENCODER: {
            struct drm_mode_get_encoder *e = arg;
            e->crtc_id = 9;
            e->possible_crtcs = 1;
            break;
        }
        case DRM_IOCTL_MODE_GETCRTC:
            ((struct drm_mode_crtc *)arg)->fb_id = 42;
            break;
        case DRM_IOCTL_MODE_CREATE_DUMB: {
            struct drm_mode_create_dumb *c = arg;
            c->handle = (uint32_t)++handles;
            c->pitch = 16;
            c->size = 64;
            break;
        }
        case DRM_IOCTL_MODE_ADDFB:
            ((struct drm_mode_fb_cmd *)arg)->fb_id = (uint32_t)++fbs;
            break;
        case DRM_IOCTL_MODE_MAP_DUMB:
            break;
        case DRM_IOCTL_MODE_DIRTYFB:
            break;
        case DRM_IOCTL_MODE_SETCRTC:
            if (((struct drm_mode_crtc *)arg)->fb_id == 42) {
                restored++;
            }
            break;
        default:
            assert(!"unexpected ioctl");
    }
    return 0;
}

int main(void) {
    for (backend = MUI_DRM; backend <= MUI_FBDEV; backend++) {
        for (int failure = 0; failure < 30; failure++) {
            calls = 0;
            fail_at = failure;
            restored = 0;
            memset(framebuffer, 0x5a, sizeof(framebuffer));
            mui_options o = {(mui_backend)backend, "/fake"};
            mui_display *d = NULL;
            if (!mui_open(&d, &o)) {
                const mui_surface *s = mui_buffer(d);
                s->pixels[0] = 0xff0000;
                mui_present(d);
                fail_at = 0;
                assert(!mui_close(d));
                if (backend == MUI_DRM) {
                    assert(restored == 1);
                }
            }
            assert(!opens && !maps && !handles && !fbs);
            for (unsigned i = 0; i < sizeof(framebuffer); i++) {
                assert(framebuffer[i] == 0x5a);
            }
        }
    }
    backend = MUI_DRM;
    fail_at = 0;
    calls = 0;
    mui_options explicit_drm = {MUI_DRM, "/fake"};
    mui_display *broken = NULL;
    assert(!mui_open(&broken, &explicit_drm));
    int front = broken->front;
    fail_at = calls + 2;
    assert(mui_present(broken) < 0);
    assert(broken->front == front);
    fail_at = calls + 1;
    assert(mui_close(broken) < 0);
    assert(!opens && !maps && !handles && !fbs);
    backend = MUI_FBDEV;
    fail_at = 0;
    calls = 0;
    mui_display *d = NULL;
    assert(!mui_open(&d, NULL));
    assert(mui_backend_used(d) == MUI_FBDEV);
    assert(!mui_close(d));
    assert(!opens && !maps);
}

int __wrap_open64(const char *path, int flags, ...) {
    return __wrap_open(path, flags);
}

void *__wrap_mmap64(void *a, size_t s, int p, int f, int fd, off64_t o) {
    return __wrap_mmap(a, s, p, f, fd, (off_t)o);
}
