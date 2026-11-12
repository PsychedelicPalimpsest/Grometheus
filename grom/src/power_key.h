/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GROM_POWER_KEY_H
#define GROM_POWER_KEY_H

#include <stdbool.h>
#include <linux/input.h>

#define GROM_POWER_KEY_DEVICES 128

typedef struct {
    int fd;
    unsigned number;
    bool down;
    bool dropping;
} grom_power_key_device;

typedef struct {
    grom_power_key_device devices[GROM_POWER_KEY_DEVICES];
} grom_power_key;

void grom_power_key_init(grom_power_key *key);
void grom_power_key_close(grom_power_key *key);
/* Find evdev devices advertising KEY_POWER; call again after hotplug. */
void grom_power_key_scan(grom_power_key *key);
/* Returns true once for a complete press and release, ignoring repeats. */
bool grom_power_key_poll(grom_power_key *key);

#endif
