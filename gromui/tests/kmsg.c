#include "kmsg.h"

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    int channel[2];
    assert(pipe(channel) == 0);
    grom_kmsg_set_fd(channel[1]);
    errno = EBUSY;
    grom_kmsg(GROM_LOG_DEBUG, "Magisk partition map has %zu entries", (size_t)2);
    assert(errno == EBUSY);

    char record[128] = {0};
    ssize_t size = read(channel[0], record, sizeof(record) - 1);
    assert(size > 0);
    assert(strcmp(record, "<7>grom: Magisk partition map has 2 entries\n") == 0);

    /* The descriptor is borrowed and remains owned by its caller. */
    assert(write(channel[1], "x", 1) == 1);
    close(channel[0]);
    close(channel[1]);
    return 0;
}
