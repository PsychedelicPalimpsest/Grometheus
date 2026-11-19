#define _GNU_SOURCE
#include "monocypher.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

int grom_create_userdata(void);
int grom_hash(const char *password, const uint8_t salt[16], uint8_t out_key[32]);
int grom_format_gcrypt(const char *block_device, const char *admin_password,
                      uint16_t slot_count);

int __real_open(const char *path, int flags, ...);
DIR *__real_opendir(const char *path);
void __real_crypto_argon2(uint8_t *hash, uint32_t hash_size, void *work_area,
                        crypto_argon2_config config, crypto_argon2_inputs inputs,
                        crypto_argon2_extras extras);

enum { FIXTURE_PATH_SIZE = 128, HEADER_SIZE = 4096 };
static char fixture[] = "/tmp/grom-disk-test-XXXXXX";
static char image_path[FIXTURE_PATH_SIZE];
static const char sys_block[] = "/sys/dev/block";
static const char password[] = "grom test password";
static const uint8_t salt[16] = "0123456789abcdef";
/* Reference output from libargon2 for 64 MiB, 3 passes, and 1 lane. */
static const uint8_t expected_hash[32] = {
    0xd8, 0x80, 0x04, 0xc8, 0xe4, 0xcf, 0x7e, 0xce,
    0x88, 0x58, 0xd9, 0x44, 0xff, 0xad, 0x36, 0xa4,
    0xdd, 0x52, 0x35, 0x1a, 0x2a, 0xf2, 0x85, 0x61,
    0x82, 0x9e, 0xd5, 0xc5, 0x67, 0x7d, 0x98, 0x0a,
};
static bool hash_computed;
static bool image_opened;
static unsigned node_calls;
static int node_error;

static void fixture_path(char path[FIXTURE_PATH_SIZE], const char *entry) {
    int length = snprintf(path, FIXTURE_PATH_SIZE, "%s%s", fixture, entry);
    assert(length >= 0 && length < FIXTURE_PATH_SIZE);
}

DIR *__wrap_opendir(const char *path) {
    assert(strcmp(path, sys_block) == 0);
    return __real_opendir(fixture);
}

int __wrap_open(const char *path, int flags, ...) {
    if (strcmp(path, "/dev/kmsg") == 0) {
        errno = ENOENT;
        return -1;
    }
    if (image_path[0] && strcmp(path, image_path) == 0) {
        assert(flags == (O_WRONLY | O_SYNC));
        image_opened = true;
        return __real_open(image_path, flags);
    }

    assert(flags == O_RDONLY);
    const char *entry = "/salt";
    if (strcmp(path, "/dev/urandom") != 0) {
        assert(strncmp(path, sys_block, sizeof(sys_block) - 1) == 0);
        entry = path + sizeof(sys_block) - 1;
        assert(entry[0] == '/');
    }
    char local[FIXTURE_PATH_SIZE];
    fixture_path(local, entry);
    return __real_open(local, flags);
}

int __wrap_mknod(const char *path, mode_t mode, dev_t device) {
    assert(strcmp(path, "/dev/userdata") == 0);
    assert(mode == (S_IFBLK | 0660));
    assert(major(device) == 179 && minor(device) == 2);
    ++node_calls;
    errno = node_error;
    return node_error ? -1 : 0;
}

void __wrap_crypto_argon2(uint8_t *hash, uint32_t hash_size, void *work_area,
                        crypto_argon2_config config, crypto_argon2_inputs inputs,
                        crypto_argon2_extras extras) {
    __real_crypto_argon2(hash, hash_size, work_area, config, inputs, extras);
    hash_computed = true;
}

static void write_fixture(const char *path, const void *data, size_t length) {
    int fd = __real_open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0);
    assert(write(fd, data, length) == (ssize_t)length);
    assert(close(fd) == 0);
}

static void test_userdata(void) {
    char other[FIXTURE_PATH_SIZE], userdata[FIXTURE_PATH_SIZE];
    char other_event[FIXTURE_PATH_SIZE], userdata_event[FIXTURE_PATH_SIZE];
    const char other_device[] = "MAJOR=179\nMINOR=1\nPARTNAME=userdata_backup\n";
    const char data_device[] = "MINOR=2\nPARTNAME=userdata\nMAJOR=179\n";
    fixture_path(other, "/179:1");
    fixture_path(other_event, "/179:1/uevent");
    fixture_path(userdata, "/179:2");
    fixture_path(userdata_event, "/179:2/uevent");

    assert(mkdir(other, 0700) == 0);
    write_fixture(other_event, other_device, sizeof(other_device) - 1);
    assert(grom_create_userdata() == 1 && node_calls == 0);

    assert(mkdir(userdata, 0700) == 0);
    write_fixture(userdata_event, data_device, sizeof(data_device) - 1);
    assert(grom_create_userdata() == 0 && node_calls == 1);
    node_error = EPERM;
    assert(grom_create_userdata() == 1 && node_calls == 2);

    assert(unlink(userdata_event) == 0 && rmdir(userdata) == 0);
    assert(unlink(other_event) == 0 && rmdir(other) == 0);
}

static void test_hash(void) {
    uint8_t hash[sizeof(expected_hash)];
    assert(grom_hash(password, salt, hash) == 0);
    assert(memcmp(hash, expected_hash, sizeof(hash)) == 0);
}

static int test_format(void) {
    char salt_path[FIXTURE_PATH_SIZE];
    uint8_t before[HEADER_SIZE + 1], after[sizeof(before)];
    fixture_path(salt_path, "/salt");
    fixture_path(image_path, "/image");
    write_fixture(salt_path, salt, sizeof(salt));
    memset(before, 0xa5, sizeof(before));
    write_fixture(image_path, before, sizeof(before));

    int result = grom_format_gcrypt(image_path, password, 16);
    int fd = __real_open(image_path, O_RDONLY);
    assert(fd >= 0);
    assert(read(fd, after, sizeof(after)) == (ssize_t)sizeof(after));
    assert(close(fd) == 0);
    assert(unlink(salt_path) == 0 && unlink(image_path) == 0);

    assert(hash_computed);
    if (result == 1 && !image_opened) {
        assert(memcmp(before, after, sizeof(before)) == 0);
        puts("The formatter exits after hashing and before opening the image.");
        return 77;
    }

    assert(result == 0 && image_opened);
    assert(memcmp(after, "GromCrypt", 9) == 0);
    assert(memmem(after, HEADER_SIZE, salt, sizeof(salt)));
    assert(memmem(after, HEADER_SIZE, expected_hash, sizeof(expected_hash)));
    assert(after[HEADER_SIZE] == before[HEADER_SIZE]);
    return 0;
}

int main(int argc, char **argv) {
    assert(mkdtemp(fixture));
    int result = 0;
    if (argc == 1) {
        test_userdata();
        test_hash();
    } else {
        assert(argc == 2 && strcmp(argv[1], "format") == 0);
        result = test_format();
    }
    assert(rmdir(fixture) == 0);
    return result;
}
