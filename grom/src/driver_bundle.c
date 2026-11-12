/* SPDX-License-Identifier: Apache-2.0 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "driver_bundle.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static int load_kernel_module(int fd) {
    return (int)syscall(SYS_finit_module, fd, "", 0);
}

static int safe_name(const char *name) {
    size_t length = strlen(name);
    if (length < 4 || length > 127 || strcmp(name + length - 3, ".ko") != 0)
        return 0;
    for (size_t i = 0; i < length; ++i) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return 0;
    }
    return name[0] != '.';
}

int grom_drivers_load_with(const char *root, int (*load_module)(int)) {
    if (!root || !load_module) {
        errno = EINVAL;
        return -1;
    }
    char manifest[256];
    if (snprintf(manifest, sizeof(manifest), "%s/modules.load", root) >=
        (int)sizeof(manifest)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int manifest_fd = open(manifest, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (manifest_fd < 0) return errno == ENOENT ? 0 : -1;
    struct stat info;
    if (fstat(manifest_fd, &info) < 0 || !S_ISREG(info.st_mode) ||
        info.st_size <= 0 || info.st_size > 8192) {
        close(manifest_fd);
        errno = EINVAL;
        return -1;
    }
    char contents[8192];
    size_t size = (size_t)info.st_size;
    size_t received = 0;
    while (received < size) {
        ssize_t got = read(manifest_fd, contents + received, size - received);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            close(manifest_fd);
            return -1;
        }
        received += (size_t)got;
    }
    close(manifest_fd);
    char name[160];
    int errors = 0;
    unsigned count = 0;
    for (size_t start = 0; start < size;) {
        size_t end = start;
        while (end < size && contents[end] != '\n') ++end;
        size_t length = end - start;
        if (end == size || length == 0 || length >= sizeof(name)) {
            fprintf(stderr, "grom: malformed driver list\n");
            errors++;
            break;
        }
        memcpy(name, contents + start, length);
        name[length] = '\0';
        start = end + 1;
        if (++count > 64 || !safe_name(name)) {
            fprintf(stderr, "grom: invalid driver name\n");
            errors++;
            break;
        }
        char path[384];
        if (snprintf(path, sizeof(path), "%s/modules/%s", root, name) >=
            (int)sizeof(path)) {
            errors++;
            break;
        }
        int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0 || fstat(fd, &info) < 0 || !S_ISREG(info.st_mode)) {
            fprintf(stderr, "grom: cannot open driver %s: %s\n", name,
                    strerror(fd < 0 ? errno : EINVAL));
            if (fd >= 0) close(fd);
            errors++;
            continue;
        }
        int result = load_module(fd);
        int saved_errno = errno;
        close(fd);
        if (result < 0 && saved_errno != EEXIST) {
            fprintf(stderr, "grom: driver %s: %s\n", name, strerror(saved_errno));
            errors++;
        }
        /* The kernel retains its own loaded module. Delete only the ramdisk
         * copy; a failed candidate will be retried on the next boot. */
        if (unlink(path) < 0) errors++;
    }
    if (unlink(manifest) < 0) errors++;
    char modules_dir[256];
    if (snprintf(modules_dir, sizeof(modules_dir), "%s/modules", root) <
        (int)sizeof(modules_dir)) rmdir(modules_dir);
    rmdir(root);
    return errors ? -1 : 0;
}

int grom_drivers_load(const char *root) {
    return grom_drivers_load_with(root, load_kernel_module);
}
