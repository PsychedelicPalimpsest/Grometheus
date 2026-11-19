#include "disk.h"
#include "kmsg.h"

#include <string.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <endian.h>
#include <sys/mman.h>


ssize_t getrandom(void* buf, size_t size, unsigned int flags);

#include "monocypher.h"
#include <monocypher-ed25519.h>

static const char GROM_MAGIC[9] = "GromCrypt";

static int parse_device_number(const char *str, size_t length) {
    if (length == 0) {
        errno = EINVAL;
        return -1;
    }

    int value = 0;
    for (size_t i = 0; i < length; i++) {
        int digit = str[i] - '0';
        if (digit < 0 || digit > 9) {
            errno = EINVAL;
            return -1;
        }
        if (value > (INT_MAX - digit) / 10) {
            errno = ERANGE;
            return -1;
        }
        value = value * 10 + digit;
    }
    return value;
}

#define __do_all(STYLE)                                                 \
    static ssize_t STYLE##_all(int fd, void *out, size_t amount) {      \
        size_t i = 0;                                                   \
        while (i < amount) {                                            \
            ssize_t count = STYLE(fd, out + i, amount - i);             \
            if (count < 0) {                                            \
                if (errno == EINTR) continue;                           \
                return -1;                                              \
            }                                                           \
            if (count == 0) break;                                      \
            i += (size_t) count;                                        \
        }                                                               \
        return (ssize_t) i;                                             \
    }                                                                   
__do_all(read)  // read_all(fd, out, amount)
__do_all(write) // write_all(fd, out, amount)

static ssize_t getrandom_all(uint8_t *out, size_t amount) {
    size_t offset = 0;
    while (offset < amount) {
        ssize_t count = getrandom(out + offset, amount - offset, 0);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (count == 0)
            break;
        offset += (size_t)count;
    }
    return (ssize_t)offset;
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
        grom_error("Cannot open /sys/dev/block, cannot find any partitions!");
        return 1;
    }

    size_t vlen = strlen(value);
    char path[64];
    char uevent[1024];

    while ((dirent = readdir(blockdir))) {
        if (dirent->d_name[0] == '.') continue;

        int ret = snprintf(path, sizeof(path), "/sys/dev/block/%s/uevent", dirent->d_name);
        if (ret < 0 || (size_t)ret >= sizeof(path))
            goto path_err;

        int fd = open(path, O_RDONLY);
        if (-1 == fd) goto path_err;

        ssize_t count = read_all(fd, uevent, sizeof(uevent));
        int read_errno = errno;
        close(fd);
        if (count < 0) {
            errno = read_errno;
            goto path_err;
        }
        size_t ueventc = (size_t)count;

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
        if (!majorstr || !minorstr) {
            errno = EINVAL;
            goto path_err;
        }

        int major_number, minor_number;
        
        if (0 > (major_number = parse_device_number(majorstr, majorc)))
            goto path_err;

        if (0 > (minor_number = parse_device_number(minorstr, minorc)))
            goto path_err;

        *major = major_number;
        *minor = minor_number;
        closedir(blockdir);
        return 0;

path_err:
        grom_error("Cannot handle block device %s; errno %s", dirent->d_name, strerror(errno));
    }

    closedir(blockdir);
    return 1;
}

static bool userdata_avalible = false;
static int userdata_device[2];

int grom_create_userdata() {
    int major, minor;
    if (search_device("PARTNAME", "userdata", &major, &minor)) {
        grom_error("Cannot find userdata device (possibly missing drivers)!");
        return 1;
    }

    grom_error("Using %d:%d for block device", major, minor);

    userdata_device[0] = major;
    userdata_device[1] = minor;
    userdata_avalible = true;

    if (mknod("/dev/userdata", S_IFBLK | 0660,  makedev(major, minor))) {
        grom_error("Cannot mount the userdata parition (%d:%d), err:%s", major, minor, strerror(errno));
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
        grom_error("Cannot allocate data for hash work area!");
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


int grypt_create_header(uint8_t sector[4096], const char* admin_password, uint16_t slot_count) {
    struct grom_userdata user_data = {
        .disk_version_major = GROM_MAJOR,
        .disk_version_minor = GROM_MINOR,
        .slot_count = htole16(slot_count),
    };
    
    memcpy(&user_data.__magic, GROM_MAGIC, sizeof(GROM_MAGIC));

    size_t salt_size = sizeof(user_data.disk_salt);
    ssize_t count = getrandom_all(user_data.disk_salt, salt_size);

    if (count != (ssize_t)salt_size) {
        grom_error("Could not read the full admin password salt");
        return 1;
    }

    if (grom_hash(admin_password, user_data.disk_salt, user_data.admin_password_hash))
        return 1;

    memcpy(sector, &user_data, sizeof(user_data));
    return 0;
}


int gcrypt_format(const char* block_device, const char* admin_password, uint16_t slot_count) {
    uint8_t sector[4096] = {0};
    int fd = open(block_device, O_WRONLY | O_SYNC);
    if (-1 == fd) {
        grom_error("Cannot open the device %s for formatting!", block_device);
        return 1;
    }

    int result = 1;

    if (grypt_create_header(sector, admin_password, slot_count))
        goto cleanup;
    if (write_all(fd, sector, sizeof(sector)) != (ssize_t)sizeof(sector)) {
        grom_error("Could not write the full grom crypt header sector");
        goto cleanup;
    }

    for (int i = 0; i < slot_count; i++) {
        if (getrandom_all(sector, sizeof(sector)) != (ssize_t)sizeof(sector) ||
            write_all(fd, sector, sizeof(sector)) != (ssize_t)sizeof(sector)) {
            grom_error("Slot randomization failed!");
            goto cleanup;
        }
    }
    result = 0;

cleanup:
    if (close(fd) != 0 && result == 0) {
        grom_error("Could not close the formatted device");
        return 1;
    }
    return result;
}

int gcrypt_load_header(int fd, struct grom_userdata* data) {
    if (-1 == lseek(fd, 0, SEEK_SET)) return 1;
    if (read_all(fd, data, sizeof(*data)) != (ssize_t)sizeof(*data)) {
        grom_error("Could not read in all the data in the grom crypt header");
        return 1;
    }
    if (0!=memcmp(data->__magic, GROM_MAGIC, sizeof(GROM_MAGIC))) {
        grom_error("Not a grom crypt partition!");
        return 1;
    }
    if (data->disk_version_major != GROM_MAJOR) {
        grom_error("Unsupported grom crypt major version %u",
                  (unsigned)data->disk_version_major);
        return 1;
    }

    // ANY 8 byte or higher value MUST be converted
    data->slot_count = le16toh(data->slot_count);
    return 0;
}


int gcrypt_decrypt_slots(const int fd, const struct grom_userdata* data, const char* password, struct grom_slot_payload*** payloads, size_t* payloadc) {
    uint8_t pre_key[32+32], key[64];

    int ret = 0;
    *payloadc = 0;

    struct grom_slot_payload* current_payload = malloc(SLOT_PAYLOAD_SIZE);
    if (!current_payload) {grom_error("OOM in slot payload allocation"); return 1;}

    
    // Load slots into ram
    struct grom_on_disk_keyslot* slots = mmap(
        NULL,
        data->slot_count * 4096,
        PROT_READ,
        MAP_PRIVATE | MAP_POPULATE,
        fd,
        4096 // Right after header
    );

    if (slots == MAP_FAILED) {
        grom_error("Failed to map in slot data");
        return 1;
    }

    grom_hash(password, data->disk_salt, pre_key);
    

    for (int i = 0; i < data->slot_count; i++) {
        crypto_sha512(key, pre_key, sizeof(pre_key));

        // Returns zero on success
        if (0 != crypto_aead_unlock(
            (uint8_t*) current_payload,
            slots[i].mac,
            key,
            slots[i].nonce,
            NULL, 0, // No additional info
            slots[i].ciphertext, sizeof(slots[i].ciphertext)
        )) continue;

        // For the sake of speed, assume slot collisions are uncommon (this code only runs once, so realloc acts like malloc)
        *payloads = realloc(*payloads, (*payloadc + 1) * sizeof(void*));

        if (!*payloads) {ret=1; grom_error("OOM in slot payload list allocation"); goto cleanup;}
        
        (*payloads)[(*payloadc)++] = current_payload;
        current_payload = malloc(SLOT_PAYLOAD_SIZE);

        if (!current_payload) {ret=1; grom_error("OOM in slot payload allocation"); goto cleanup;}
        
    }

    
cleanup:
    // Both of these may have secrets
    crypto_wipe(pre_key, sizeof(pre_key));
    crypto_wipe(key, sizeof(key));
    free(current_payload);
   
    munmap(slots, data->slot_count * 4096);
    return ret;
}



// Return true of the admin password was verified, 0 if not, -1 on error
int gcrypt_verify_password(struct grom_userdata* data, const char* admin_password) {
    uint8_t test_hash[32];
    if (grom_hash(admin_password, data->disk_salt, test_hash)) {
        grom_error("Error hashing the admin password");
        return -1;
    }

    return 0 == crypto_verify32(test_hash, data->admin_password_hash);
}
