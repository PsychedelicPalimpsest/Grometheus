#include "disk.h"
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
#include "monocypher.h"

static const char GROM_MAGIC[9] = "GromCrypt";

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

static int read_all(int fd, void* out, size_t amount) {
    for (size_t i = 0; i < amount;) {
        ssize_t red = read(fd, out+i, amount-i);
        if (red==-1 && errno == EINTR) {errno=0; continue;}
        if (red==-1) return -1;
        if (red==0) return i;

        i += red;
    }
    return 0;
}

static int write_all(int fd, void* buff, size_t amount) {
    for (size_t i = 0; i < amount;) {
        ssize_t written = write(fd, buff+i, amount-i);
        if (written==-1 && errno == EINTR) {errno=0; continue;}
        if (written==-1) return -1;
        if (written==0) return i;

        i += written;
    }
    return 0;
}

static void search_uevent(const char* uevent, size_t ueventc, const char* key, char const** value, size_t* vlen) {
    size_t klen = strlen(key);
    for (size_t i = 0; i < ueventc;) {
        if (i + klen < ueventc
            && 0 == strncmp(key, uevent + i, klen)
            && uevent[i + klen] == '=') {
            
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
        
        size_t ueventc = read_all(fd, uevent, sizeof(uevent)-1);
        
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


// TODO: Find the highest numbers that 'feel' right on a phone and/or make a config option
const crypto_argon2_config grom_argoncfg = {
        .algorithm = CRYPTO_ARGON2_ID, /* Argon2id variant */
        .nb_blocks = 65536,            /* 65536 * 1024 bytes = 64 MiB */
        .nb_passes = 3,                /* 3 iterations (minimum recommended) */
        .nb_lanes  = 1                 /* Monocypher is single-threaded */
    };

int grom_hash(const char *password, const uint8_t salt[16], uint8_t out_key[32])
{
    size_t work_area_size = (size_t)grom_argoncfg.nb_blocks * 1024;
    void *work_area = malloc(work_area_size);
    if (!work_area) {
        grom_kmsg(GROM_LOG_ERROR, "Cannot allocate data for hash work area!");
        return 1;
    }

    size_t pass_len = strlen(password);
    crypto_argon2_inputs inputs = {
        .pass      = (const uint8_t *)password,
        .salt      = salt,
        .pass_size = (uint32_t)pass_len,
        .salt_size = 16
    };

    crypto_argon2(out_key, 32, work_area, grom_argoncfg, inputs, crypto_argon2_no_extras);

    free(work_area);
    return 0;
}


int grom_format_gcrypt(const char* block_device, const char* admin_password, uint16_t slot_count) {
    struct grom_userdata user_data = {
        .disk_version_major = GROM_MAJOR,
        .disk_version_minor = GROM_MINOR,
        .slot_count = slot_count,
    };
    
    memcpy(&user_data.__magic, GROM_MAGIC, sizeof(GROM_MAGIC));
    int rand_fd = open("/dev/urandom", 0);
    if (rand_fd == -1) {
        grom_kmsg(GROM_LOG_ERROR, "Cannot open urandom!");
        return 1;
    }
    read_all(rand_fd, user_data.admin_password_salt, sizeof(user_data.admin_password_salt));
    close(rand_fd);

    if (!grom_hash(admin_password, user_data.admin_password_salt, user_data.admin_password_hash))
        return 1;

    uint8_t grom_data[4096] = {0};
    memcpy(grom_data, &user_data, sizeof(struct grom_userdata));
    
    


    int fd = open(block_device, O_WRONLY | O_SYNC);
    if (-1 == fd) {
        grom_kmsg(GROM_LOG_ERROR, "Cannot open the device %s for formatting!", block_device);
        return 1;
    }

    write_all(fd, grom_data, sizeof(grom_data));
    close(fd);
    
        
    return 0;
}



