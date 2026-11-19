#define _GNU_SOURCE
#include "disk.h"
#include "monocypher.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

int search_device(const char *key, const char *value, int *major, int *minor);
int grom_hash(const char *password, const uint8_t salt[16], uint8_t out_key[32]);
int grypt_create_header(uint8_t sector[4096], const char *admin_password, uint16_t slot_count);
int gcrypt_format(const char *block_device, const char *admin_password, uint16_t slot_count);
int gcrypt_load_header(int fd, struct grom_userdata *data);
int gcrypt_verify_password(struct grom_userdata *data, const char *admin_password);
extern const crypto_argon2_config grom_argoncfg;

int __real_open(const char *path, int flags, ...);
DIR *__real_opendir(const char *path);
ssize_t __real_read(int fd, void *buffer, size_t size);
ssize_t __real_write(int fd, const void *buffer, size_t size);
void *__real_malloc(size_t size);

enum { SECTOR_SIZE = 4096, SLOT_COUNT = 2, IMAGE_SIZE = (1 + SLOT_COUNT) * SECTOR_SIZE + 1 };
enum write_fault { WRITE_OK, WRITE_ERROR, WRITE_SHORT, WRITE_ZERO_HEADER };

static const char sys_block[] = "/sys/dev/block";
static const char password[] = "grom test password";
static const uint8_t salt[16] = {
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
};
/* libargon2 reference for 64 MiB, 3 passes and 1 lane. */
static const uint8_t expected_hash[32] = {
    0xd8, 0x80, 0x04, 0xc8, 0xe4, 0xcf, 0x7e, 0xce,
    0x88, 0x58, 0xd9, 0x44, 0xff, 0xad, 0x36, 0xa4,
    0xdd, 0x52, 0x35, 0x1a, 0x2a, 0xf2, 0x85, 0x61,
    0x82, 0x9e, 0xd5, 0xc5, 0x67, 0x7d, 0x98, 0x0a,
};

static char fixture[] = "/tmp/grom-disk-test-XXXXXX";
static char image_path[128];
static char event_path[128];
static char device_path[128];

static uint8_t entropy[sizeof(salt) + SLOT_COUNT * SECTOR_SIZE];
static size_t entropy_size, entropy_offset, entropy_chunk_size;
static unsigned entropy_calls, fail_entropy_call, entropy_interrupts;
static int failures;
static int image_fd = -1, event_fd = -1;
static int read_fault_fd = -1;
static bool directory_error, event_read_error, fail_hash_allocation;
static bool header_read_error;
static unsigned hash_allocations;
static unsigned node_calls, image_writes, slot_entropy_calls, read_interrupts;
static size_t read_limit;
static enum write_fault write_fault;
static int node_error;
static dev_t node_device;

static void expect(bool ok, const char *message) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

static bool fd_is_closed(int fd) {
    return fd < 0 || (fcntl(fd, F_GETFD) == -1 && errno == EBADF);
}

static void fixture_path(char path[128], const char *name) {
    int size = snprintf(path, 128, "%s/%s", fixture, name);
    assert(size >= 0 && size < 128);
}

DIR *__wrap_opendir(const char *path) {
    assert(strcmp(path, sys_block) == 0);
    if (directory_error) {
        errno = ENOENT;
        return NULL;
    }
    return __real_opendir(fixture);
}

int __wrap_open(const char *path, int flags, ...) {
    if (strcmp(path, image_path) == 0) {
        assert(flags == (O_WRONLY | O_SYNC));
        image_fd = __real_open(path, flags);
        return image_fd;
    }
    assert(flags == O_RDONLY);
    const char prefix[] = "/sys/dev/block/";
    assert(strncmp(path, prefix, sizeof(prefix) - 1) == 0);
    char local[128];
    fixture_path(local, path + sizeof(prefix) - 1);
    event_fd = __real_open(local, flags);
    return event_fd;
}

int __wrap_mknod(const char *path, mode_t mode, dev_t device) {
    assert(strcmp(path, "/dev/userdata") == 0);
    assert(mode == (S_IFBLK | 0660));
    ++node_calls;
    node_device = device;
    if (node_error) {
        errno = node_error;
        return -1;
    }
    return 0;
}

ssize_t __wrap_getrandom(void *buffer, size_t size, unsigned int flags) {
    assert(flags == 0);
    assert(entropy_offset <= entropy_size);
    if (entropy_offset >= sizeof(salt))
        ++slot_entropy_calls;
    if (++entropy_calls == fail_entropy_call) {
        errno = EIO;
        return -1;
    }
    if (entropy_interrupts) {
        --entropy_interrupts;
        errno = EINTR;
        return -1;
    }
    if (size > entropy_size - entropy_offset)
        size = entropy_size - entropy_offset;
    if (size > entropy_chunk_size)
        size = entropy_chunk_size;
    memcpy(buffer, entropy + entropy_offset, size);
    entropy_offset += size;
    return (ssize_t)size;
}

ssize_t __wrap_read(int fd, void *buffer, size_t size) {
    if ((event_read_error && fd == event_fd) ||
        (header_read_error && fd == read_fault_fd)) {
        errno = EIO;
        return -1;
    }
    if (fd == read_fault_fd) {
        if (read_interrupts) {
            --read_interrupts;
            errno = EINTR;
            return -1;
        }
        if (read_limit && size > read_limit)
            size = read_limit;
    }
    return __real_read(fd, buffer, size);
}

ssize_t __wrap_write(int fd, const void *buffer, size_t size) {
    if (fd == image_fd) {
        ++image_writes;
        if (write_fault == WRITE_ERROR) {
            errno = EIO;
            return -1;
        }
        if (write_fault == WRITE_SHORT) {
            if (image_writes == 1) {
                assert(size > 16);
                return __real_write(fd, buffer, 16);
            }
            return 0;
        }
        if (write_fault == WRITE_ZERO_HEADER && image_writes == 1)
            return 0;
    }
    return __real_write(fd, buffer, size);
}

void *__wrap_malloc(size_t size) {
    if (size == (size_t)grom_argoncfg.nb_blocks * 1024) {
        ++hash_allocations;
        if (fail_hash_allocation) {
            errno = ENOMEM;
            return NULL;
        }
    }
    return __real_malloc(size);
}

static void write_fixture(const char *path, const void *data, size_t size) {
    int fd = __real_open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    assert(fd >= 0);
    assert(__real_write(fd, data, size) == (ssize_t)size);
    assert(close(fd) == 0);
}

static void write_event(const char *contents) {
    if (mkdir(device_path, 0700) != 0)
        assert(errno == EEXIST);
    write_fixture(event_path, contents, strlen(contents));
}

static void test_lookup(void) {
    write_event("MAJOR=179\nMINOR=1\nPARTNAME=userdata_backup\n");
    expect(grom_create_userdata() == 1 && node_calls == 0, "reject a partial partition name");

    write_event("MINOR=x\nPARTNAME=userdata\nMAJOR=179\n");
    expect(grom_create_userdata() == 1 && node_calls == 0, "reject a non-numeric minor");

    write_event("MINOR=2\nPARTNAME=userdata\nMAJOR=179");
    expect(grom_create_userdata() == 0 && node_calls == 1, "create the matching block node");
    expect(major(node_device) == 179 && minor(node_device) == 2, "pass the discovered device number");
    node_error = EPERM;
    expect(grom_create_userdata() == 1 && node_calls == 2, "propagate mknod failure");
}

static void test_lookup_open_error(void) {
    directory_error = true;
    int result = grom_create_userdata();
    expect(result == 1, "report failure when /sys/dev/block cannot be opened");
    expect(node_calls == 0, "do not call mknod without a discovered device");
}

static void test_lookup_invalid_numbers(void) {
    int major_number, minor_number;
    write_event("MAJOR=\nMINOR=2\nPARTNAME=userdata\n");
    expect(search_device("PARTNAME", "userdata", &major_number, &minor_number) != 0,
           "reject an empty major number");
    write_event("MAJOR=179\nMINOR=-2\nPARTNAME=userdata\n");
    expect(search_device("PARTNAME", "userdata", &major_number, &minor_number) != 0,
           "reject a negative minor number");
}

static void test_lookup_overflow(void) {
    int major_number, minor_number;
    write_event("MAJOR=2147483648\nMINOR=2\nPARTNAME=userdata\n");
    expect(search_device("PARTNAME", "userdata", &major_number, &minor_number) != 0,
           "reject a device number larger than INT_MAX");
}

static void test_lookup_read_error(void) {
    write_event("MAJOR=179\nMINOR=2\nPARTNAME=userdata\n");
    /* Keep fixture cleanup in the parent if the scan crashes. */
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        int major_number, minor_number;
        event_read_error = true;
        int result = search_device("PARTNAME", "userdata", &major_number, &minor_number);
        expect(result != 0, "skip a uevent when reading it fails");
        expect(fd_is_closed(event_fd), "close the uevent descriptor after a read failure");
        _exit(failures ? EXIT_FAILURE : EXIT_SUCCESS);
    }

    int status;
    assert(waitpid(child, &status, 0) == child);
    if (WIFSIGNALED(status))
        fprintf(stderr, "uevent scan terminated by signal %d\n", WTERMSIG(status));
    expect(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS,
           "handle a uevent read failure");
}

/* Build the fixture in wire order independently of the loader and creator. */
static void make_header(uint8_t *bytes, uint16_t slots) {
    _Static_assert(sizeof(struct grom_userdata) == 61, "header fixture must match the wire size");
    memcpy(bytes, "GromCrypt", 9);
    bytes[9] = GROM_MAJOR;
    bytes[10] = GROM_MINOR;
    bytes[11] = (uint8_t)slots;
    bytes[12] = (uint8_t)(slots >> 8);
    memcpy(bytes + 13, salt, sizeof(salt));
    memcpy(bytes + 29, expected_hash, sizeof(expected_hash));
}

static void test_hash_and_verify(void) {
    uint8_t hash[32] = {0};
    expect(grom_hash(password, salt, hash) == 0, "derive the Argon2id verifier");
    expect(memcmp(hash, expected_hash, sizeof(hash)) == 0, "match the libargon2 reference");

    struct grom_userdata header;
    make_header((uint8_t *)&header, SLOT_COUNT);
    expect(gcrypt_verify_password(&header, password) == 1, "accept the correct credential");
    expect(gcrypt_verify_password(&header, "wrong password") == 0, "reject a wrong credential");
    fail_hash_allocation = true;
    expect(gcrypt_verify_password(&header, password) == -1, "report a derivation error separately");
}

static int open_header(const uint8_t *bytes, size_t size) {
    write_fixture(image_path, bytes, size);
    int fd = __real_open(image_path, O_RDONLY);
    assert(fd >= 0);
    return fd;
}

static void test_load(void) {
    uint8_t bytes[SECTOR_SIZE] = {0};
    make_header(bytes, 0x0102);
    int fd = open_header(bytes, sizeof(bytes));
    assert(lseek(fd, 100, SEEK_SET) == 100);
    read_fault_fd = fd;
    read_interrupts = 1;
    read_limit = 7;

    struct grom_userdata header;
    int result = gcrypt_load_header(fd, &header);
    expect(result == 0, "load a valid header after EINTR and short reads");
    expect(result == 0 && header.slot_count == 0x0102, "decode the little-endian slot count");
    expect(result == 0 && memcmp(header.admin_password_hash, expected_hash, 32) == 0,
           "load the verifier from offset zero");
    header_read_error = true;
    expect(gcrypt_load_header(fd, &header) != 0, "reject an I/O error while reading the header");
    read_fault_fd = -1;
    assert(close(fd) == 0);

    fd = open_header(bytes, sizeof(header) - 1);
    expect(gcrypt_load_header(fd, &header) == 1, "reject a truncated header");
    assert(close(fd) == 0);

    int ends[2];
    assert(pipe(ends) == 0);
    expect(gcrypt_load_header(ends[0], &header) == 1, "reject a non-seekable descriptor");
    assert(close(ends[0]) == 0 && close(ends[1]) == 0);
}

static void test_invalid_header(void) {
    struct grom_userdata header;
    uint8_t bytes[SECTOR_SIZE] = {0};
    make_header(bytes, SLOT_COUNT);
    bytes[0] ^= 1;
    int fd = open_header(bytes, sizeof(bytes));
    expect(gcrypt_load_header(fd, &header) != 0, "reject a header with the wrong magic");
    assert(close(fd) == 0);

    make_header(bytes, SLOT_COUNT);
    bytes[9] ^= 1;
    fd = open_header(bytes, sizeof(bytes));
    expect(gcrypt_load_header(fd, &header) != 0, "reject an unsupported major version");
    assert(close(fd) == 0);
}

static void test_header_create(void) {
    entropy_interrupts = 1;
    entropy_chunk_size = 7;
    uint8_t sector[SECTOR_SIZE] = {0}, expected[sizeof(struct grom_userdata)];
    make_header(expected, 0x0102);
    expect(grypt_create_header(sector, password, 0x0102) == 0,
           "create a header after interrupted and short entropy reads");
    expect(memcmp(sector, expected, sizeof(expected)) == 0,
           "serialize magic, version, little-endian slot count, salt and verifier");
}

static void test_header_hash_error(void) {
    uint8_t before[SECTOR_SIZE], sector[SECTOR_SIZE];
    memset(before, 0xa5, sizeof(before));
    memcpy(sector, before, sizeof(sector));
    fail_hash_allocation = true;
    expect(grypt_create_header(sector, password, SLOT_COUNT) != 0, "reject a failed hash allocation");
    expect(memcmp(sector, before, sizeof(sector)) == 0, "preserve the output on hashing failure");
}

static void test_header_entropy_error(void) {
    uint8_t sector[SECTOR_SIZE] = {0};
    fail_entropy_call = 1;
    expect(grypt_create_header(sector, password, SLOT_COUNT) != 0 && hash_allocations == 0,
           "reject a salt entropy error before hashing");

    entropy_size = sizeof(salt) - 1;
    expect(grypt_create_header(sector, password, SLOT_COUNT) != 0,
           "reject a partial salt followed by zero progress");
    expect(hash_allocations == 0, "do not hash with an incomplete salt");
}

static void prepare_image(void) {
    uint8_t before[IMAGE_SIZE];
    memset(before, 0xa5, sizeof(before));
    write_fixture(image_path, before, sizeof(before));
}

static void reset_format_io(void) {
    if (!fd_is_closed(image_fd))
        assert(close(image_fd) == 0);
    image_fd = -1;
    entropy_calls = fail_entropy_call = entropy_interrupts = 0;
    image_writes = slot_entropy_calls = 0;
    entropy_offset = 0;
    entropy_size = entropy_chunk_size = sizeof(entropy);
}

static void read_image(uint8_t image[IMAGE_SIZE]) {
    int fd = __real_open(image_path, O_RDONLY);
    assert(fd >= 0);
    assert(__real_read(fd, image, IMAGE_SIZE) == IMAGE_SIZE);
    uint8_t extra;
    expect(__real_read(fd, &extra, 1) == 0, "preserve the image length");
    assert(close(fd) == 0);
}

static void test_format(void) {
    uint8_t image[IMAGE_SIZE], header[SECTOR_SIZE] = {0};
    prepare_image();
    expect(gcrypt_format(image_path, password, SLOT_COUNT) == 0, "format the disposable image");
    expect(fd_is_closed(image_fd), "close the image descriptor");
    read_image(image);
    make_header(header, SLOT_COUNT);
    expect(memcmp(image, header, sizeof(header)) == 0, "write the full padded header");
    expect(image[IMAGE_SIZE - 1] == 0xa5, "preserve the byte beyond the slot region");
}

static void test_format_open_error(void) {
    expect(gcrypt_format(image_path, password, SLOT_COUNT) != 0, "reject failure to open the image");
    expect(entropy_calls == 0, "skip entropy requests when the image open fails");
    reset_format_io();

    prepare_image();
    fail_entropy_call = 1;
    expect(gcrypt_format(image_path, password, SLOT_COUNT) != 0, "report a header entropy error");
    expect(image_writes == 0, "leave the image untouched when header entropy fails");
    expect(fd_is_closed(image_fd), "close the image descriptor");
}

static void test_format_header_write(void) {
    prepare_image();

    expect(gcrypt_format(image_path, password, SLOT_COUNT) == 0,
           "continue to slots after a complete header write");
    expect(fd_is_closed(image_fd), "close the image descriptor");

    reset_format_io();
    write_fault = WRITE_ERROR;
    expect(gcrypt_format(image_path, password, SLOT_COUNT) != 0, "report a header write returning EIO");
    expect(image_writes == 1 && slot_entropy_calls == 0, "stop after the failing header write");
    expect(fd_is_closed(image_fd), "close the image descriptor");

    reset_format_io();
    write_fault = WRITE_SHORT;
    expect(gcrypt_format(image_path, password, SLOT_COUNT) != 0, "report a partial header write");
    expect(image_writes == 2 && slot_entropy_calls == 0, "stop when the partial header write stalls");
    expect(fd_is_closed(image_fd), "close the image descriptor");

    reset_format_io();
    write_fault = WRITE_ZERO_HEADER;
    expect(gcrypt_format(image_path, password, SLOT_COUNT) != 0, "report zero progress writing the header");
    expect(image_writes == 1 && slot_entropy_calls == 0, "stop after the zero-progress header write");
    expect(fd_is_closed(image_fd), "close the image descriptor");
}

static void test_format_slots(void) {
    uint8_t image[IMAGE_SIZE], expected[SLOT_COUNT * SECTOR_SIZE];
    prepare_image();
    expect(gcrypt_format(image_path, password, SLOT_COUNT) == 0, "accept full slot entropy reads and writes");
    expect(slot_entropy_calls == SLOT_COUNT && image_writes == 1 + SLOT_COUNT,
           "attempt both slot writes after reading their entropy");
    expect(fd_is_closed(image_fd), "close the image descriptor");
    read_image(image);
    memset(expected, 0x3c, SECTOR_SIZE);
    memset(expected + SECTOR_SIZE, 0x71, SECTOR_SIZE);
    expect(memcmp(image + SECTOR_SIZE, expected, sizeof(expected)) == 0,
           "write distinct entropy into both slot sectors");
    expect(image[IMAGE_SIZE - 1] == 0xa5, "preserve the byte beyond the slot region");
}

static void test_format_slot_entropy_error(void) {
    prepare_image();
    fail_entropy_call = 2;
    expect(gcrypt_format(image_path, password, SLOT_COUNT) != 0, "report a slot entropy error");
    expect(slot_entropy_calls == 1, "stop on the first slot with missing entropy");
    expect(image_writes == 1, "leave the slot untouched when entropy fails");
    expect(fd_is_closed(image_fd), "close the image descriptor");

    reset_format_io();
    entropy_size = sizeof(salt) + 16;
    expect(gcrypt_format(image_path, password, SLOT_COUNT) != 0,
           "reject partial slot entropy followed by zero progress");
    expect(image_writes == 1, "leave the slot untouched when entropy is incomplete");
    expect(fd_is_closed(image_fd), "close the image descriptor");
}

int main(int argc, char **argv) {
    const struct {
        const char *name;
        void (*run)(void);
    } cases[] = {
        {"lookup", test_lookup},
        {"lookup-open-error", test_lookup_open_error},
        {"lookup-invalid-numbers", test_lookup_invalid_numbers},
        {"lookup-overflow", test_lookup_overflow},
        {"lookup-read-error", test_lookup_read_error},
        {"hash-and-verify", test_hash_and_verify},
        {"load", test_load},
        {"invalid-header", test_invalid_header},
        {"header-create", test_header_create},
        {"header-hash-error", test_header_hash_error},
        {"header-entropy-error", test_header_entropy_error},
        {"format", test_format},
        {"format-open-error", test_format_open_error},
        {"format-header-write", test_format_header_write},
        {"format-slots", test_format_slots},
        {"format-slot-entropy-error", test_format_slot_entropy_error},
    };

    assert(argc == 2);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (strcmp(argv[1], cases[i].name) != 0)
            continue;
        assert(mkdtemp(fixture));
        fixture_path(image_path, "image");
        fixture_path(device_path, "179:2");
        fixture_path(event_path, "179:2/uevent");
        memcpy(entropy, salt, sizeof(salt));
        memset(entropy + sizeof(salt), 0x3c, SECTOR_SIZE);
        memset(entropy + sizeof(salt) + SECTOR_SIZE, 0x71, SECTOR_SIZE);
        reset_format_io();
        cases[i].run();
        reset_format_io();
        assert(unlink(event_path) == 0 || errno == ENOENT);
        assert(rmdir(device_path) == 0 || errno == ENOENT);
        assert(unlink(image_path) == 0 || errno == ENOENT);
        assert(rmdir(fixture) == 0);
        return failures ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    fprintf(stderr, "Unknown disk test: %s\n", argv[1]);
    return EXIT_FAILURE;
}
