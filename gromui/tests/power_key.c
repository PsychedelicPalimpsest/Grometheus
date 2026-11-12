/* SPDX-License-Identifier: Apache-2.0 */
#include "power_key.h"
#include "power_request.h"
#include "grom_backend.h"

#undef NDEBUG
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

static void send_event(int fd, uint16_t type, uint16_t code, int value) {
    struct input_event event = {.type = type, .code = code, .value = value};
    assert(write(fd, &event, sizeof(event)) == sizeof(event));
}

int main(void) {
    int input[2];
    assert(pipe(input) == 0);
    assert(fcntl(input[0], F_SETFL, O_NONBLOCK) == 0);
    grom_power_key key;
    grom_power_key_init(&key);
    key.devices[0].fd = input[0];

    /* A release or key repeat after attach is not an intentional tap. */
    send_event(input[1], EV_KEY, KEY_POWER, 0);
    send_event(input[1], EV_KEY, KEY_POWER, 2);
    assert(!grom_power_key_poll(&key));
    send_event(input[1], EV_KEY, KEY_POWER, 1);
    send_event(input[1], EV_KEY, KEY_POWER, 2);
    assert(!grom_power_key_poll(&key));
    send_event(input[1], EV_KEY, KEY_POWER, 0);
    assert(grom_power_key_poll(&key));
    assert(!grom_power_key_poll(&key));

    /* A lost evdev frame cannot complete an old press. */
    send_event(input[1], EV_KEY, KEY_POWER, 1);
    send_event(input[1], EV_SYN, SYN_DROPPED, 0);
    send_event(input[1], EV_KEY, KEY_POWER, 0);
    send_event(input[1], EV_SYN, SYN_REPORT, 0);
    assert(!grom_power_key_poll(&key));
    send_event(input[1], EV_KEY, KEY_POWER, 1);
    send_event(input[1], EV_KEY, KEY_POWER, 0);
    assert(grom_power_key_poll(&key));
    close(input[1]);
    assert(!grom_power_key_poll(&key));
    assert(key.devices[0].fd == -1);
    grom_power_key_close(&key);

    int request[2];
    assert(pipe(request) == 0);
    grom_backend_set_power_fd(request[1]);
    assert(grom_backend_power(GROM_POWER_SHUTDOWN) == GROM_BACKEND_OK);
    assert(grom_backend_power(GROM_POWER_REBOOT) == GROM_BACKEND_OK);
    char bytes[2];
    assert(read(request[0], bytes, sizeof(bytes)) == sizeof(bytes));
    assert(bytes[0] == 'S' && bytes[1] == 'R');
    assert(grom_backend_power((grom_power_action)100) == GROM_BACKEND_INVALID_INPUT);
    grom_backend_set_power_fd(-1);
    assert(grom_backend_power(GROM_POWER_SHUTDOWN) == GROM_BACKEND_UNAVAILABLE);
    close(request[0]);
    close(request[1]);
    puts("power key: pass");
    return 0;
}
