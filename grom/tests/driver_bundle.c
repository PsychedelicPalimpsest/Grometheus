#define _GNU_SOURCE
#include "driver_bundle.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned calls;

static int fake_load(int fd) {
    char value;
    assert(read(fd, &value, 1) == 1);
    ++calls;
    if (value == 'b') {
        errno = ENODEV;
        return -1;
    }
    return 0;
}

int main(void) {
    char path[] = "/tmp/grom-driver-test-XXXXXX";
    assert(mkdtemp(path));
    char modules[256], list[256], first[512], second[512];
    snprintf(modules, sizeof(modules), "%s/modules", path);
    snprintf(list, sizeof(list), "%s/modules.load", path);
    snprintf(first, sizeof(first), "%s/a.ko", modules);
    snprintf(second, sizeof(second), "%s/b.ko", modules);
    assert(mkdir(modules, 0700) == 0);
    FILE *file = fopen(list, "w");
    assert(file);
    assert(fputs("a.ko\nb.ko\n", file) >= 0);
    assert(fclose(file) == 0);
    file = fopen(first, "w");
    assert(file && fputs("a", file) >= 0 && fclose(file) == 0);
    file = fopen(second, "w");
    assert(file && fputs("b", file) >= 0 && fclose(file) == 0);
    assert(grom_drivers_load_with(path, fake_load) == -1);
    assert(calls == 2);
    assert(access(first, F_OK) < 0 && access(second, F_OK) < 0);
    assert(access(list, F_OK) < 0);
    assert(grom_drivers_load_with(path, fake_load) == 0);
    return 0;
}
