#include "kmsg.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

static int kmsg_fd = -1;

void grom_kmsg_set_fd(int fd) {
    kmsg_fd = fd;
}

void grom_kmsg(int priority, const char *format, ...) {
    int saved_errno = errno;
    char record[512];
    if (priority < 0 || priority > 7) priority = GROM_LOG_INFO;
    int prefix = snprintf(record, sizeof(record), "<%d>grom: ", priority);
    if (prefix < 0 || (size_t)prefix >= sizeof(record) - 2) goto done;

    va_list args;
    va_start(args, format);
    int length = vsnprintf(record + prefix, sizeof(record) - (size_t)prefix - 1,
                           format, args);
    va_end(args);
    if (length < 0) goto done;

    size_t used = (size_t)prefix + (size_t)length;
    if (used >= sizeof(record) - 1) used = sizeof(record) - 2;
    if (used > 0 && record[used - 1] == '\n') --used;
    record[used++] = '\n';

    int fd = kmsg_fd;
    int close_fd = 0;
    if (fd < 0) {
        fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
        close_fd = 1;
    }
    if (fd >= 0) {
        (void)write(fd, record, used);
        if (close_fd) close(fd);
    }
done:
    errno = saved_errno;
}
