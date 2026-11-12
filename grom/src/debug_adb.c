#include "debug_adb.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

#define GADGET "/config/usb_gadget/grom"
extern char **environ;

static int contains(const char *haystack, const char *needle) {
    size_t length = strlen(needle);
    for (; *haystack; ++haystack) {
        size_t i = 0;
        while (i < length && haystack[i] && haystack[i] == needle[i]) ++i;
        if (i == length) return 1;
    }
    return 0;
}

static void report_message(const char *message) {
    size_t n = strlen(message);
    int fd = open("/grom/adb-status", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) { write(fd, message, n); close(fd); }
    fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) { write(fd, message, n); close(fd); }
}

static void report_detail(const char *what, const char *kind, int code) {
    char message[160];
    int n = snprintf(message, sizeof(message), "grom adb: %s (%s %d)\n",
                     what, kind, code);
    if (n > 0 && n < (int)sizeof(message)) report_message(message);
}

static void report_status(const char *what, int code) {
    report_detail(what, "errno", code);
}

static void log_error(const char *what) { report_status(what, errno); }

static int make_dir(const char *path, mode_t mode) {
    return mkdir(path, mode) == 0 || errno == EEXIST;
}

static int make_char_device(const char *path, unsigned major, unsigned minor,
                            mode_t mode) {
    dev_t device = makedev(major, minor);
    if (mknod(path, S_IFCHR | mode, device) == 0) return 1;
    if (errno != EEXIST) return 0;
    struct stat current;
    return stat(path, &current) == 0 && S_ISCHR(current.st_mode) &&
           current.st_rdev == device;
}

static int setup_standard_devices(void) {
    if (!make_dir("/dev", 0755) ||
        !make_char_device("/dev/null", 1, 3, 0666) ||
        !make_char_device("/dev/zero", 1, 5, 0666) ||
        !make_char_device("/dev/random", 1, 8, 0666) ||
        !make_char_device("/dev/urandom", 1, 9, 0666) ||
        !make_char_device("/dev/tty", 5, 0, 0666)) {
        log_error("create ADB standard devices");
        return 0;
    }
    return 1;
}

static int write_value(const char *path, const char *value) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    size_t length = strlen(value);
    int ok = write(fd, value, length) == (ssize_t)length;
    close(fd);
    return ok;
}

static int find_udc(char *name, size_t size) {
    DIR *dir = opendir("/sys/class/udc");
    if (!dir) return 0;
    struct dirent *entry;
    int found = 0;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.' || contains(entry->d_name, "dummy")) continue;
        if (strlen(entry->d_name) >= size) continue;
        strcpy(name, entry->d_name);
        found = 1;
        break;
    }
    closedir(dir);
    return found;
}

static int setup_usb(void) {
    if (!make_dir("/config", 0755)) return 0;
    if (mount("configfs", "/config", "configfs", 0, NULL) && errno != EBUSY) {
        log_error("mount configfs");
        return 0;
    }
    if (!make_dir("/config/usb_gadget", 0755) ||
        !make_dir(GADGET, 0755) ||
        !make_dir(GADGET "/strings", 0755) ||
        !make_dir(GADGET "/strings/0x409", 0755) ||
        !make_dir(GADGET "/configs", 0755) ||
        !make_dir(GADGET "/configs/b.1", 0755) ||
        !make_dir(GADGET "/configs/b.1/strings", 0755) ||
        !make_dir(GADGET "/configs/b.1/strings/0x409", 0755) ||
        !make_dir(GADGET "/functions", 0755) ||
        !make_dir(GADGET "/functions/ffs.adb", 0755)) {
        log_error("create USB gadget");
        return 0;
    }
    if (!write_value(GADGET "/idVendor", "0x18d1") ||
        !write_value(GADGET "/idProduct", "0x4ee7") ||
        !write_value(GADGET "/strings/0x409/serialnumber", "grom") ||
        !write_value(GADGET "/strings/0x409/manufacturer", "grom") ||
        !write_value(GADGET "/strings/0x409/product", "grom preboot") ||
        !write_value(GADGET "/configs/b.1/strings/0x409/configuration", "adb")) {
        log_error("configure USB gadget");
        return 0;
    }
    if (symlink(GADGET "/functions/ffs.adb", GADGET "/configs/b.1/f1") &&
        errno != EEXIST) {
        log_error("link USB ADB function");
        return 0;
    }
    if (!make_dir("/dev/usb-ffs", 0770) ||
        !make_dir("/dev/usb-ffs/adb", 0770) ||
        (mount("adb", "/dev/usb-ffs/adb", "functionfs", 0,
               "uid=2000,gid=1000,rmode=0770,fmode=0660") && errno != EBUSY)) {
        log_error("mount ADB FunctionFS");
        return 0;
    }
    chown("/dev/usb-ffs", 2000, 1000);
    chown("/dev/usb-ffs/adb", 2000, 1000);
    if (access("/dev/usb-ffs/adb/ep0", F_OK)) {
        log_error("ADB FunctionFS ep0 missing");
        return 0;
    }
    return 1;
}

static int adbd_opened_ep0(pid_t pid) {
    char directory[64];
    snprintf(directory, sizeof(directory), "/proc/%ld/fd", (long)pid);
    DIR *fds = opendir(directory);
    if (!fds) return 0;
    int found = 0;
    struct dirent *entry;
    while ((entry = readdir(fds))) {
        if (entry->d_name[0] == '.') continue;
        char path[128], target[256];
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        ssize_t n = readlink(path, target, sizeof(target) - 1);
        if (n <= 0) continue;
        target[n] = 0;
        if (contains(target, "/dev/usb-ffs/adb/ep0")) { found = 1; break; }
    }
    closedir(fds);
    return found;
}

int grom_debug_adb_start(void) {
    if (access("/grom/debug-adb", R_OK) ||
        access("/system/bin/adbd", X_OK)) return 0;

    char udc[128];
    if (!find_udc(udc, sizeof(udc))) {
        report_status("no USB device controller in /sys/class/udc", 0);
        return -1;
    }
    make_dir("/data", 0700);
    make_dir("/data/adb", 0700);
    make_dir("/tmp", 01777);
    make_dir("/dev/socket", 0775);
    if (!setup_standard_devices()) return -1;
    if (!setup_usb()) return -1;
    make_dir("/dev/pts", 0755);
    if (mount("devpts", "/dev/pts", "devpts", 0,
              "mode=0620,ptmxmode=0666") && errno != EBUSY)
        log_error("mount devpts");
    if (access("/dev/ptmx", F_OK)) symlink("pts/ptmx", "/dev/ptmx");
    if (access("/bin", F_OK)) symlink("/system/bin", "/bin");
    if (!make_dir("/sbin", 0755) ||
        (symlink("/system/bin/sh", "/sbin/sh") && errno != EEXIST) ||
        access("/sbin/sh", X_OK)) {
        log_error("install ADB shell link");
        return -1;
    }
    unlink("/grom/ffs-ready");

    pid_t child = fork();
    if (child < 0) { log_error("fork adbd"); return -1; }
    if (child == 0) {
        syscall(SYS_setsid);
        int in = open("/dev/null", O_RDONLY);
        int out = open("/grom/adbd.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (in >= 0) syscall(SYS_dup3, in, STDIN_FILENO, 0);
        if (out >= 0) {
            syscall(SYS_dup3, out, STDOUT_FILENO, 0);
            syscall(SYS_dup3, out, STDERR_FILENO, 0);
            write(out, "grom: launching adbd\n", 21);
        }
        if (in > 2) close(in);
        if (out > 2) close(out);
        setenv("LD_LIBRARY_PATH", "/system/lib64:/system/lib", 1);
        setenv("LD_PRELOAD", "/system/lib64/libgrom_adb_shim.so", 1);
        setenv("PATH", "/system/bin:/bin", 1);
        setenv("ANDROID_ROOT", "/system", 1);
        setenv("ANDROID_DATA", "/data", 1);
        setenv("LD_CONFIG_FILE", "/system/etc/ld.config.txt", 1);
        setenv("HOME", "/", 1);
        char *const args[] = {"/system/bin/adbd", "--device_banner=grom", NULL};
        execve(args[0], args, environ);
        _exit(127);
    }
    int ready = 0;
    for (unsigned i = 0; i < 100; ++i) {
        int status = 0;
        if (syscall(SYS_wait4, child, &status, WNOHANG, NULL) == child) {
            if (WIFEXITED(status))
                report_detail("adbd exited before opening ep0", "exit code", WEXITSTATUS(status));
            else if (WIFSIGNALED(status))
                report_detail("adbd exited before opening ep0", "signal", WTERMSIG(status));
            else
                report_detail("adbd exited before opening ep0", "wait status", status);
            return -1;
        }
        if (adbd_opened_ep0(child) && access("/grom/ffs-ready", R_OK) == 0) {
            ready = 1;
            break;
        }
        usleep(100000);
    }
    if (!ready) { report_status("adbd did not signal FunctionFS ready", 0); return -1; }
    if (!write_value(GADGET "/UDC", udc)) {
        log_error("bind USB gadget");
        return -1;
    }
    report_status("USB ADB bound", 0);
    return 1;
}
