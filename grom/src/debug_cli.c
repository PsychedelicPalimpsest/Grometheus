/* Static command installed only in opt-in debug ADB ramdisks. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "debug_ui_protocol.h"

#include <errno.h>
#include <limits.h>
#include <linux/reboot.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

static int transfer(int from, int to) {
    unsigned char buffer[16384];
    for (;;) {
        ssize_t n = read(from, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        if (n == 0) return 0;
        size_t done = 0;
        while (done < (size_t)n) {
            ssize_t written = write(to, buffer + done, (size_t)n - done);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) return -1;
            done += (size_t)written;
        }
    }
}

static int parse_coordinate(const char *value, uint32_t *out) {
    if (!value[0] || value[0] == '-') return -1;
    char *end = NULL;
    errno = 0;
    unsigned long number = strtoul(value, &end, 10);
    if (errno || *end || number > UINT32_MAX) return -1;
    *out = (uint32_t)number;
    return 0;
}

static int request_ui(uint32_t operation, uint32_t x, uint32_t y) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    memcpy(address.sun_path, GROM_DEBUG_UI_SOCKET, sizeof(GROM_DEBUG_UI_SOCKET));
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror(GROM_DEBUG_UI_SOCKET);
        close(fd);
        return 1;
    }
    grom_debug_ui_request request = {
        .magic = GROM_DEBUG_UI_MAGIC,
        .operation = operation,
        .x = x,
        .y = y,
    };
    if (write(fd, &request, sizeof(request)) != sizeof(request)) {
        perror("grom-debug");
        close(fd);
        return 1;
    }
    unsigned char prefix[3];
    ssize_t n = recv(fd, prefix, sizeof(prefix), MSG_WAITALL);
    int valid = n == (ssize_t)sizeof(prefix) &&
                ((operation == GROM_DEBUG_UI_TAP && memcmp(prefix, "OK ", 3) == 0) ||
                 (operation == GROM_DEBUG_UI_SCREENSHOT && memcmp(prefix, "P6\n", 3) == 0));
    int output = valid ? STDOUT_FILENO : STDERR_FILENO;
    if (n > 0 && write(output, prefix, (size_t)n) != n) valid = 0;
    if (transfer(fd, output) < 0) {
        perror("grom-debug");
        close(fd);
        return 1;
    }
    close(fd);
    return valid ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "tap") == 0) {
        uint32_t x, y;
        if (parse_coordinate(argv[2], &x) || parse_coordinate(argv[3], &y)) {
            fprintf(stderr, "tap needs nonnegative integer x and y\n");
            return 2;
        }
        return request_ui(GROM_DEBUG_UI_TAP, x, y);
    }
    if (argc == 2 && strcmp(argv[1], "screenshot") == 0)
        return request_ui(GROM_DEBUG_UI_SCREENSHOT, 0, 0);
    if (argc == 2 && strcmp(argv[1], "bootloader") == 0) {
        syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
                LINUX_REBOOT_CMD_RESTART2, "bootloader");
        perror("reboot bootloader");
        return 1;
    }
    fprintf(stderr, "usage: grom-debug tap X Y | screenshot | bootloader\n");
    return 2;
}
