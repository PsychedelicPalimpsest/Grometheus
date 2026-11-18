#ifndef GROM_KMSG_H
#define GROM_KMSG_H

enum {
    GROM_LOG_ERROR = 3,
    GROM_LOG_INFO = 6,
    GROM_LOG_DEBUG = 7,
};

/* Borrow MagiskInit's open kernel-log descriptor before entering preboot.
 * Set once before the renderer is forked; grom does not close it. */
void grom_kmsg_set_fd(int fd);

/* Writes one grom-prefixed kernel log record. Logging is best effort. */
void grom_kmsg(int priority, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

#endif
