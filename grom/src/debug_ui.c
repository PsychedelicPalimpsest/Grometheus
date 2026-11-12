/* Debug-only control of the actual preboot renderer. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "debug_ui.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint32_t elapsed_ms(const struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)((now.tv_sec - start->tv_sec) * 1000 +
                      (now.tv_nsec - start->tv_nsec) / 1000000);
}

static int send_all(int fd, const void *data, size_t size) {
    const unsigned char *p = data;
    while (size) {
        ssize_t n = syscall(SYS_sendto, fd, p, size, MSG_NOSIGNAL, NULL, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        size -= (size_t)n;
    }
    return 0;
}

static void screenshot(int client, const gromui_surface *surface) {
    char header[48];
    int count = snprintf(header, sizeof(header), "P6\n%u %u\n255\n",
                         surface->width, surface->height);
    if (count <= 0 || count >= (int)sizeof(header) ||
        send_all(client, header, (size_t)count) < 0) return;
    unsigned char *row = malloc((size_t)surface->width * 3);
    if (!row) return;
    for (uint32_t y = 0; y < surface->height; ++y) {
        const uint32_t *pixels = (const uint32_t *)((const unsigned char *)surface->pixels +
                                                    (size_t)y * surface->stride);
        for (uint32_t x = 0; x < surface->width; ++x) {
            uint32_t pixel = pixels[x];
            row[(size_t)x * 3] = (unsigned char)(pixel >> 16);
            row[(size_t)x * 3 + 1] = (unsigned char)(pixel >> 8);
            row[(size_t)x * 3 + 2] = (unsigned char)pixel;
        }
        if (send_all(client, row, (size_t)surface->width * 3) < 0) break;
    }
    free(row);
}

void grom_debug_ui_start(grom_debug_ui *debug) {
    memset(debug, 0, sizeof(*debug));
    debug->listener = -1;
    if (access("/grom/debug-adb", R_OK) != 0) return;
    int fd = syscall(SYS_socket, AF_UNIX,
                     SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return;
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    memcpy(address.sun_path, GROM_DEBUG_UI_SOCKET, sizeof(GROM_DEBUG_UI_SOCKET));
    unlink(GROM_DEBUG_UI_SOCKET);
    if (syscall(SYS_bind, fd, &address,
                offsetof(struct sockaddr_un, sun_path) + sizeof(GROM_DEBUG_UI_SOCKET)) < 0 ||
        chmod(GROM_DEBUG_UI_SOCKET, 0600) < 0 || syscall(SYS_listen, fd, 2) < 0) {
        close(fd);
        unlink(GROM_DEBUG_UI_SOCKET);
        return;
    }
    debug->listener = fd;
}

void grom_debug_ui_stop(grom_debug_ui *debug) {
    if (debug->listener >= 0) {
        close(debug->listener);
        unlink(GROM_DEBUG_UI_SOCKET);
    }
    debug->listener = -1;
}

void grom_debug_ui_poll(grom_debug_ui *debug, lv_indev_t *indev,
                        lv_display_t *display, gromui_display *backend) {
    if (debug->listener < 0) return;
    while (syscall(SYS_wait4, -1, NULL, WNOHANG, NULL) > 0) {}
    int client = syscall(SYS_accept4, debug->listener, NULL, NULL, SOCK_CLOEXEC);
    if (client < 0) return;
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 200000};
    syscall(SYS_setsockopt, client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    grom_debug_ui_request request;
    ssize_t n = syscall(SYS_recvfrom, client, &request, sizeof(request),
                        MSG_WAITALL, NULL, NULL);
    if (n != sizeof(request) || request.magic != GROM_DEBUG_UI_MAGIC) {
        send_all(client, "invalid request\n", 16);
        close(client);
        return;
    }
    const gromui_surface *surface = gromui_buffer(backend);
    if (request.operation == GROM_DEBUG_UI_TAP) {
        if (request.x >= surface->width || request.y >= surface->height) {
            send_all(client, "tap outside display\n", 20);
        } else {
            struct timespec start;
            clock_gettime(CLOCK_MONOTONIC, &start);
            lv_indev_reset(indev, NULL);
            debug->x = (int32_t)request.x;
            debug->y = (int32_t)request.y;
            debug->injecting = true;
            debug->pressed = true;
            lv_indev_read(indev);
            const struct timespec held = {.tv_sec = 0, .tv_nsec = 120000000};
            nanosleep(&held, NULL);
            debug->pressed = false;
            lv_indev_read(indev);
            debug->injecting = false;
            uint32_t input_ms = elapsed_ms(&start);
            lv_refr_now(display);
            char response[80];
            int length = snprintf(response, sizeof(response),
                                  "OK input_ms=%u render_ms=%u\n",
                                  input_ms, elapsed_ms(&start) - input_ms);
            if (length > 0 && length < (int)sizeof(response))
                send_all(client, response, (size_t)length);
        }
    } else if (request.operation == GROM_DEBUG_UI_SCREENSHOT) {
        lv_refr_now(display);
        pid_t child = fork();
        if (child == 0) {
            close(debug->listener);
            timeout.tv_sec = 3;
            timeout.tv_usec = 0;
            syscall(SYS_setsockopt, client, SOL_SOCKET, SO_SNDTIMEO,
                    &timeout, sizeof(timeout));
            screenshot(client, surface);
            close(client);
            _exit(0);
        }
        if (child < 0) send_all(client, "screenshot fork failed\n", 23);
    } else {
        send_all(client, "unknown operation\n", 18);
    }
    close(client);
}
