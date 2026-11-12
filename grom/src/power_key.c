/* SPDX-License-Identifier: Apache-2.0 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "power_key.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define KEY_WORDS (KEY_MAX / (sizeof(unsigned long) * CHAR_BIT) + 1)

void grom_power_key_init(grom_power_key *key) {
    memset(key, 0, sizeof(*key));
    for(unsigned i = 0; i < GROM_POWER_KEY_DEVICES; ++i)
        key->devices[i].fd = -1;
}

void grom_power_key_close(grom_power_key *key) {
    for(unsigned i = 0; i < GROM_POWER_KEY_DEVICES; ++i) {
        if(key->devices[i].fd >= 0) close(key->devices[i].fd);
        key->devices[i].fd = -1;
    }
}

void grom_power_key_scan(grom_power_key *key) {
    DIR *dir = opendir("/dev/input");
    if(!dir) return;
    struct dirent *entry;
    while((entry = readdir(dir))) {
        if(memcmp(entry->d_name, "event", 5) != 0) continue;
        const char *digit = entry->d_name + 5;
        if(*digit < '0' || *digit > '9') continue;
        unsigned number = 0;
        for(; *digit >= '0' && *digit <= '9'; ++digit) {
            number = number * 10 + (unsigned)(*digit - '0');
            if(number >= 128) break;
        }
        if(*digit || number >= 128) continue;
        unsigned free_slot = GROM_POWER_KEY_DEVICES;
        bool known = false;
        for(unsigned i = 0; i < GROM_POWER_KEY_DEVICES; ++i) {
            if(key->devices[i].fd >= 0 && key->devices[i].number == number)
                known = true;
            if(free_slot == GROM_POWER_KEY_DEVICES && key->devices[i].fd < 0)
                free_slot = i;
        }
        if(known || free_slot == GROM_POWER_KEY_DEVICES) continue;
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%u", number);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if(fd < 0) continue;
        unsigned long bits[KEY_WORDS] = {0};
        if(ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0 ||
           !(bits[KEY_POWER / (sizeof(unsigned long) * CHAR_BIT)] &
             (1UL << (KEY_POWER % (sizeof(unsigned long) * CHAR_BIT))))) {
            close(fd);
            continue;
        }
        key->devices[free_slot] = (grom_power_key_device){.fd = fd, .number = number};
    }
    closedir(dir);
}

static bool feed(grom_power_key_device *device, const struct input_event *event) {
    if(event->type == EV_SYN && event->code == SYN_DROPPED) {
        device->down = false;
        device->dropping = true;
        return false;
    }
    if(device->dropping) {
        if(event->type == EV_SYN && event->code == SYN_REPORT)
            device->dropping = false;
        return false;
    }
    if(event->type != EV_KEY || event->code != KEY_POWER) return false;
    if(event->value == 1) {
        device->down = true;
    } else if(event->value == 0) {
        bool completed = device->down;
        device->down = false;
        return completed;
    }
    return false;
}

bool grom_power_key_poll(grom_power_key *key) {
    bool pressed = false;
    struct pollfd ready[GROM_POWER_KEY_DEVICES];
    unsigned index[GROM_POWER_KEY_DEVICES];
    nfds_t count = 0;
    for(unsigned i = 0; i < GROM_POWER_KEY_DEVICES; ++i) {
        if(key->devices[i].fd < 0) continue;
        ready[count] = (struct pollfd){.fd = key->devices[i].fd, .events = POLLIN};
        index[count++] = i;
    }
    if(!count || poll(ready, count, 0) <= 0) return false;
    for(nfds_t i = 0; i < count; ++i) {
        if(!ready[i].revents) continue;
        grom_power_key_device *device = &key->devices[index[i]];
        struct input_event events[16];
        for(;;) {
            ssize_t bytes = read(device->fd, events, sizeof(events));
            if(bytes > 0 && bytes % (ssize_t)sizeof(events[0]) == 0) {
                size_t event_count = (size_t)bytes / sizeof(events[0]);
                for(size_t j = 0; j < event_count; ++j)
                    pressed |= feed(device, &events[j]);
            } else if(bytes < 0 && errno == EINTR) {
                continue;
            } else if(bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            } else {
                close(device->fd);
                *device = (grom_power_key_device){.fd = -1};
                break;
            }
        }
    }
    return pressed;
}
