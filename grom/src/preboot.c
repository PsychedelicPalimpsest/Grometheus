#include "grom_preboot.h"
#include "test_ui.h"
#include "driver_bundle.h"
#include "debug_adb.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/poll.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int grom_preboot_run(void) {
    /* Credential input, charger policy and slot mapping are not implemented.
     * Never hand off an unselected raw pool to MagiskInit or stock init. */
    /* Modules are global kernel state. Load before forking the renderer, and
     * remove only the copied ramdisk files after each insertion attempt. */
    if (access("/grom/debug-adb", R_OK) == 0) {
        int kmsg = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
        if (kmsg >= 0) {
            syscall(SYS_dup3, kmsg, STDERR_FILENO, 0);
            close(kmsg);
        }
    }
    grom_debug_adb_start();
    if (grom_drivers_load("/grom/drivers") < 0)
        fprintf(stderr, "grom: some staged drivers failed to load\n");
    int ready[2];
    if (syscall(SYS_pipe2, ready, O_CLOEXEC) < 0) {
        return -1;
    }

    pid_t child = fork();
    if (child == 0) {
        close(ready[0]);
        _exit(grom_test_ui_run(ready[1]));
    }
    close(ready[1]);
    if (child < 0) {
        close(ready[0]);
        return -1;
    }

    /* A renderer stuck in a kernel ioctl cannot hold PID 1 or trigger boot.
     * The child must confirm a real display present within 20 seconds. */
    struct pollfd rendered = {.fd = ready[0], .events = POLLIN};
    char marker;
    int outcome;
    do {
        outcome = poll(&rendered, 1, 20000);
    } while (outcome < 0 && errno == EINTR);
    if (outcome != 1 || !(rendered.revents & POLLIN) || read(ready[0], &marker, 1) != 1) {
        syscall(SYS_kill, child, SIGKILL);
        for (unsigned retry = 0; retry < 20; ++retry) {
            if (syscall(SYS_wait4, child, NULL, WNOHANG, NULL) == child) {
                break;
            }
            const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000000};
            nanosleep(&delay, NULL);
        }
        close(ready[0]);
        return -1;
    }
    close(ready[0]);

    /* Wait for renderer failure without a syscall every second. PID 1 only
     * resumes MagiskInit after a real slot selection, which is not wired yet. */
    long result;
    do {
        result = syscall(SYS_wait4, child, NULL, 0, NULL);
    } while (result < 0 && errno == EINTR);
    return -1;
}
