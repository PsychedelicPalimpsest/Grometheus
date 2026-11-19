#include "kmsg.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

void grom_kmsg(int priority, const char *format, ...) {
    int saved_errno = errno;
    fprintf(stderr, "<%d>grom: ", priority);

    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);

    fputc('\n', stderr);
    errno = saved_errno;
}
