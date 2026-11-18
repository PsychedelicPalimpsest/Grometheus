#include "kmsg.h"

#include <string.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdbool.h>


static int parse_int(const char* str, size_t strc) {
    int value = 0;
    int sign = 1;

    
    if (strc && *str == '-') {
        str++; strc--;
        sign = -1;
    } else if (strc && *str == '+') {
        str++; strc--;
    }
    while (strc--) {
        char c = *(str++);
        if ('0' > c || c > '9') {
            errno = EINVAL;
            return 0;
        }
        value = value*10 + (c-'0');
    }
    return value * sign;
}

static void search_uevent(const char* uevent, size_t ueventc, const char* key, char const** value, size_t* vlen) {
    size_t klen = strlen(key);
    for (size_t i = 0; i < ueventc;) {
        if (i + klen < ueventc && 0 == strncmp(key, uevent + i, klen) && uevent[i + klen] == '=') {
            i += klen + 1;
            size_t starti = i;
            *value = uevent + i;

            while (i < ueventc && '\n' != uevent[i]) i++;
            *vlen = i - starti;
            return;
        }
        while (i < ueventc && '\n' != uevent[i]) i++;
        if (i < ueventc) i++; // Step past '\n'
    }
    *value = NULL;
    *vlen = 0;
}

int search_device(const char* key, const char* value, int* major, int* minor) {
    DIR* blockdir = opendir("/sys/dev/block");
    struct dirent* dirent;

    if (!blockdir) {
        grom_kmsg(GROM_LOG_ERROR, "Cannot open /sys/dev/block, cannot find any partitions!");
        return -1;
    }

    size_t vlen = strlen(value);
    char path[64];
    char uevent[1024];

    while ((dirent = readdir(blockdir))) {
        if (dirent->d_name[0] == '.') continue;

        int ret = snprintf(path, sizeof(path), "/sys/dev/block/%s/uevent", dirent->d_name);
        if (ret < 0 || (size_t)ret >= sizeof(path))
            goto path_err;

        int fd = open(path, 0);
        if (-1 == fd) goto path_err;
        
        size_t ueventc = read(fd, uevent, sizeof(uevent)-1);
        uevent[ueventc] = '\0';
        close(fd);

        char const* test_value;
        size_t test_value_len;
        
        search_uevent(uevent, ueventc, key, &test_value, &test_value_len);
        if (!test_value) continue;

        if (test_value_len != vlen
            || 0 != strncmp(test_value, value, vlen)) continue;

        size_t majorc, minorc;
        char const *majorstr, *minorstr;

        search_uevent(uevent, ueventc, "MAJOR", &majorstr, &majorc);
        search_uevent(uevent, ueventc, "MINOR", &minorstr, &minorc);
        if (!majorstr || !minorstr) goto path_err;

        errno = 0;
        *major = parse_int(majorstr, majorc);
        *minor = parse_int(minorstr, minorc);
        if (errno) goto path_err;
        
        closedir(blockdir);
        return 1;

path_err:
        grom_kmsg(GROM_LOG_ERROR, "Cannot handle block device %s; errno %s", dirent->d_name, strerror(errno));
    }

    closedir(blockdir);
    return 0;
}

static bool userdata_avalible = false;
static int userdata_device[2];

int grom_create_userdata() {
    int major, minor;
    if (!search_device("PARTNAME", "userdata", &major, &minor)) {
        grom_kmsg(GROM_LOG_ERROR, "Cannot find userdata device (possibly missing drivers)!");
        return 1;
    }

    grom_kmsg(GROM_LOG_ERROR, "Using %d:%d for block device", major, minor);

    userdata_device[0] = major;
    userdata_device[1] = minor;
    userdata_avalible = true;

    if (mknod("/dev/userdata", S_IFBLK | 0660,  makedev(major, minor))) {
        grom_kmsg(GROM_LOG_ERROR, "Cannot mount the userdata parition (%d:%d), err:%s", major, minor, strerror(errno));
        return 1;
    }

    return 0;
    
}
