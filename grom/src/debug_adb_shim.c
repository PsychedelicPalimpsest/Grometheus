/*
 * Minimal property shim for the bundled recovery adbd before Android init.
 *
 * Intercepts Android Bionic property queries and privileged operations so that
 * adbd and diagnostics can run in a standalone initramfs without property_service.
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>

typedef void (*prop_read_cb)(void* cookie, const char* name, const char* value, unsigned serial);

extern char** environ;

struct fake_prop {
    unsigned int serial;
    const char* name;
    const char* value;
};

static const struct fake_prop props[] = {
    {1, "service.adb.tcp.port", "-1"},
    {1, "persist.adb.tcp.port", "-1"},
    {1, "ro.adb.secure", "0"},
    {1, "ro.secure", "0"},
    {1, "ro.debuggable", "1"},
    {1, "service.adb.listen_all", "0"},
    {1, "service.adb.root", "1"},
    {1, "ro.boot.qemu", "0"},
    {1, "ro.kernel.qemu", "0"},
    {1, "sys.usb.config", "adb"},
    {1, "sys.usb.state", "adb"},
    {1, "ro.product.device", "grom"},
    {1, "ro.product.model", "grom"},
    {1, "ro.build.type", "userdebug"},
    {1, "persist.logd.security", "0"},
    {1, "ro.organization_owned", "0"},
    {1, "logd.ready", "true"},
    {1, "log.tag", "V"},
    {0, 0, 0}
};

static const struct fake_prop prop_qemu_1 = {1, "ro.boot.qemu", "1"};
static const struct fake_prop prop_kqemu_1 = {1, "ro.kernel.qemu", "1"};

static int is_qemu_env(void) {
    if (!environ) return 0;
    for (char** env = environ; *env; env++) {
        if ((*env)[0] == 'G' && (*env)[1] == 'R' && (*env)[2] == 'O' &&
            (*env)[3] == 'M' && (*env)[4] == '_' && (*env)[5] == 'Q' &&
            (*env)[6] == 'E' && (*env)[7] == 'M' && (*env)[8] == 'U' &&
            (*env)[9] == '=' && (*env)[10] == '1') {
            return 1;
        }
    }
    return 0;
}

static int my_strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static unsigned long my_strlen(const char* s) {
    unsigned long len = 0;
    while (*s++) len++;
    return len;
}

static char* my_strcpy(char* dest, const char* src) {
    char* d = dest;
    while ((*d++ = *src++));
    return dest;
}

const void* __system_property_find(const char* name) {
    if (!name) return (const void*)0;
    if (is_qemu_env()) {
        if (my_strcmp(name, "ro.boot.qemu") == 0) return (const void*)&prop_qemu_1;
        if (my_strcmp(name, "ro.kernel.qemu") == 0) return (const void*)&prop_kqemu_1;
    }
    for (int i = 0; props[i].name; i++) {
        if (my_strcmp(props[i].name, name) == 0) {
            return (const void*)&props[i];
        }
    }
    return (const void*)0;
}

unsigned int __system_property_serial(const void* pi) {
    if (!pi) return 0;
    return ((const struct fake_prop*)pi)->serial;
}

unsigned int __system_property_area_serial(void) {
    return 1;
}

void __system_property_read_callback(const void* pi, prop_read_cb callback, void* cookie) {
    if (!pi || !callback) return;
    const struct fake_prop* p = (const struct fake_prop*)pi;
    if (p->name) {
        callback(cookie, p->name, p->value, p->serial);
    }
}

int __system_property_get(const char* name, char* value) {
    if (!name || !value) return 0;
    for (int i = 0; props[i].name; i++) {
        if (my_strcmp(props[i].name, name) == 0) {
            my_strcpy(value, props[i].value);
            return (int)my_strlen(props[i].value);
        }
    }
    value[0] = '\0';
    return 0;
}

int __system_property_read(const void* pi, char* name, char* value) {
    if (!pi) return 0;
    const struct fake_prop* p = (const struct fake_prop*)pi;
    if (p->name) {
        if (name) my_strcpy(name, p->name);
        if (value) my_strcpy(value, p->value);
        return (int)my_strlen(p->value);
    }
    return 0;
}

int __system_property_wait(const void* pi, unsigned int old_serial, unsigned int* new_serial_ptr, const void* relative_timeout) {
    (void)pi;
    (void)old_serial;
    if (new_serial_ptr) *new_serial_ptr = 1;
    (void)relative_timeout;
    return 1;
}

int __system_property_set(const char* key, const char* value) {
    if (key && value && my_strcmp(key, "sys.usb.ffs.ready") == 0 &&
        my_strcmp(value, "1") == 0) {
        int fd = open("/grom/ffs-ready", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            write(fd, "1", 1);
            close(fd);
        }
    }
    return 0;
}

int __system_property_add(const char* name, unsigned int namelen,
                          const char* value, unsigned int valuelen) {
    (void)name;
    (void)namelen;
    (void)value;
    (void)valuelen;
    return 0;
}

int SetTaskProfiles(int tid, const void* profiles, int use_fd_cache) {
    (void)tid;
    (void)profiles;
    (void)use_fd_cache;
    return 1;
}

int SetProcessProfiles(int uid, int pid, const void* profiles) {
    (void)uid;
    (void)pid;
    (void)profiles;
    return 1;
}

int cap_set_proc(void* cap_p) {
    (void)cap_p;
    return 0;
}

int capset(void* hdrp, const void* datap) {
    (void)hdrp;
    (void)datap;
    return 0;
}

int setgroups(size_t size, const gid_t* list) {
    (void)size;
    (void)list;
    return 0;
}

int setgid(unsigned int gid) {
    (void)gid;
    return 0;
}

int setuid(unsigned int uid) {
    (void)uid;
    return 0;
}
