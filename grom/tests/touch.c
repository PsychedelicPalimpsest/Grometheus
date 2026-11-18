/* SPDX-License-Identifier: Apache-2.0 */
#include "touch.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int mock_touch_fd = -1;
static int mock_current_slot;

int __real_ioctl(int fd, unsigned long request, ...);

int __wrap_ioctl(int fd, unsigned long request, ...) {
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);
    if(fd == mock_touch_fd) {
        if(request == EVIOCGMTSLOTS(4 * sizeof(int32_t))) {
            int32_t *ids = arg;
            for(unsigned i = 1; i <= 3; ++i) ids[i] = -1;
            return 0;
        }
        if(request == EVIOCGABS(ABS_MT_SLOT)) {
            struct input_absinfo *slot = arg;
            memset(slot, 0, sizeof(*slot));
            slot->value = mock_current_slot;
            return 0;
        }
        assert(!"unexpected touch ioctl");
    }
    return __real_ioctl(fd, request, arg);
}

static int send_event(grom_touch *touch, uint16_t type, uint16_t code, int value,
                      grom_touch_point *point) {
    struct input_event event = {.type = type, .code = code, .value = value};
    return grom_touch_feed(touch, &event, point);
}

static void init(grom_touch *touch, grom_touch_protocol protocol) {
    grom_touch_init(touch, 101, 201);
    touch->protocol = protocol;
    if(protocol == GROM_TOUCH_MT_B) touch->slot_count = GROM_TOUCH_SLOTS;
    touch->blocked = false;
    touch->axis_x.minimum = 100;
    touch->axis_x.maximum = 1100;
    touch->axis_y.minimum = 200;
    touch->axis_y.maximum = 2200;
}

static void type_b(void) {
    grom_touch touch;
    grom_touch_point point = {0};
    init(&touch, GROM_TOUCH_MT_B);
    send_event(&touch, EV_ABS, ABS_MT_SLOT, 0, &point);
    send_event(&touch, EV_ABS, ABS_MT_TRACKING_ID, 7, &point);
    send_event(&touch, EV_ABS, ABS_MT_POSITION_X, 600, &point);
    send_event(&touch, EV_ABS, ABS_MT_POSITION_Y, 1200, &point);
    assert(send_event(&touch, EV_SYN, SYN_REPORT, 0, &point) == 1);
    assert(point.pressed && point.x == 50 && point.y == 100);

    send_event(&touch, EV_ABS, ABS_MT_SLOT, 1, &point);
    send_event(&touch, EV_ABS, ABS_MT_TRACKING_ID, 8, &point);
    send_event(&touch, EV_ABS, ABS_MT_POSITION_X, 1100, &point);
    send_event(&touch, EV_ABS, ABS_MT_POSITION_Y, 2200, &point);
    assert(send_event(&touch, EV_SYN, SYN_REPORT, 0, &point) == 1);
    assert(!point.pressed && touch.cancel_pending && touch.blocked);
    assert(touch.point.x == 50 && touch.point.y == 100);

    send_event(&touch, EV_ABS, ABS_MT_SLOT, 0, &point);
    send_event(&touch, EV_ABS, ABS_MT_TRACKING_ID, -1, &point);
    assert(send_event(&touch, EV_SYN, SYN_REPORT, 0, &point) == 0);
    assert(!point.pressed && point.x == 50 && point.y == 100);
    send_event(&touch, EV_ABS, ABS_MT_SLOT, 1, &point);
    send_event(&touch, EV_ABS, ABS_MT_POSITION_X, 800, &point);
    assert(send_event(&touch, EV_SYN, SYN_REPORT, 0, &point) == 0);
    assert(!touch.point.pressed);
    send_event(&touch, EV_ABS, ABS_MT_TRACKING_ID, -1, &point);
    send_event(&touch, EV_SYN, SYN_REPORT, 0, &point);
    assert(!touch.blocked);

    send_event(&touch, EV_ABS, ABS_MT_SLOT, 2, &point);
    send_event(&touch, EV_ABS, ABS_MT_TRACKING_ID, 9, &point);
    send_event(&touch, EV_ABS, ABS_MT_POSITION_X, 200, &point);
    send_event(&touch, EV_ABS, ABS_MT_POSITION_Y, 300, &point);
    assert(send_event(&touch, EV_SYN, SYN_REPORT, 0, &point) == 1);
    assert(point.pressed && point.x == 10 && point.y == 10);
    send_event(&touch, EV_ABS, ABS_MT_TRACKING_ID, 10, &point);
    send_event(&touch, EV_ABS, ABS_MT_POSITION_X, 1100, &point);
    send_event(&touch, EV_ABS, ABS_MT_POSITION_Y, 2200, &point);
    assert(send_event(&touch, EV_SYN, SYN_REPORT, 0, &point) == 1);
    assert(!point.pressed && touch.blocked && touch.cancel_pending);
    send_event(&touch, EV_ABS, ABS_MT_TRACKING_ID, -1, &point);
    send_event(&touch, EV_SYN, SYN_REPORT, 0, &point);
    assert(!touch.blocked);
    send_event(&touch, EV_ABS, ABS_MT_TRACKING_ID, 11, &point);
    send_event(&touch, EV_ABS, ABS_MT_POSITION_X, 200, &point);
    send_event(&touch, EV_ABS, ABS_MT_POSITION_Y, 300, &point);
    assert(send_event(&touch, EV_SYN, SYN_REPORT, 0, &point) == 1);
    assert(send_event(&touch, EV_SYN, SYN_DROPPED, 0, &point) == 1);
    assert(!point.pressed && touch.blocked && touch.cancel_pending);
    grom_touch_close(&touch);
}

/* The Moto G Fast Himax driver reports X/Y before TRACKING_ID on down, then
 * pressure-only frames until release. A stationary tap must still click. */
static void type_b_position_before_id(void) {
    grom_touch touch;
    grom_touch_point point = {0};
    init(&touch, GROM_TOUCH_MT_B);
    touch.slot_count = 1;
    send_event(&touch, EV_ABS, ABS_MT_POSITION_X, 600, &point);
    send_event(&touch, EV_ABS, ABS_MT_POSITION_Y, 1200, &point);
    send_event(&touch, EV_ABS, ABS_MT_TRACKING_ID, 30, &point);
    assert(send_event(&touch, EV_SYN, SYN_REPORT, 0, &point) == 1);
    assert(point.pressed && point.x == 50 && point.y == 100);
    send_event(&touch, EV_ABS, ABS_MT_PRESSURE, 50, &point);
    assert(send_event(&touch, EV_SYN, SYN_REPORT, 0, &point) == 0);
    assert(touch.point.pressed);
    send_event(&touch, EV_ABS, ABS_MT_TRACKING_ID, -1, &point);
    assert(send_event(&touch, EV_SYN, SYN_REPORT, 0, &point) == 1);
    assert(!point.pressed);

    send_event(&touch, EV_ABS, ABS_MT_TRACKING_ID, 31, &point);
    assert(send_event(&touch, EV_SYN, SYN_REPORT, 0, &point) == 0);
    assert(!touch.point.pressed); /* Do not reuse the prior contact's point. */
    send_event(&touch, EV_ABS, ABS_MT_POSITION_X, 700, &point);
    send_event(&touch, EV_ABS, ABS_MT_POSITION_Y, 1300, &point);
    assert(send_event(&touch, EV_SYN, SYN_REPORT, 0, &point) == 1);
    assert(point.pressed && point.x == 60 && point.y == 110);
}

/* After SYN_DROPPED, the next contact can use the kernel's current slot
 * without sending ABS_MT_SLOT again. */
static void type_b_resync_current_slot(void) {
    int pipefd[2];
    assert(pipe(pipefd) == 0);
    grom_touch touch;
    grom_touch_point point = {0};
    init(&touch, GROM_TOUCH_MT_B);
    touch.slot_count = 3;
    touch.fd = pipefd[0];
    mock_touch_fd = pipefd[0];
    mock_current_slot = 2;
    assert(send_event(&touch, EV_SYN, SYN_DROPPED, 0, &point) == 0);
    assert(send_event(&touch, EV_SYN, SYN_REPORT, 0, &point) == 0);
    assert(!touch.blocked && touch.current_slot == 2);
    send_event(&touch, EV_ABS, ABS_MT_TRACKING_ID, 41, &point);
    send_event(&touch, EV_ABS, ABS_MT_POSITION_X, 600, &point);
    send_event(&touch, EV_ABS, ABS_MT_POSITION_Y, 1200, &point);
    assert(send_event(&touch, EV_SYN, SYN_REPORT, 0, &point) == 1);
    assert(point.pressed && point.x == 50 && point.y == 100);
    assert(touch.primary_slot == 2);

    mock_current_slot = -1;
    assert(send_event(&touch, EV_SYN, SYN_DROPPED, 0, &point) == 1);
    assert(send_event(&touch, EV_SYN, SYN_REPORT, 0, &point) == 0);
    assert(!touch.blocked && touch.current_slot == -1);
    send_event(&touch, EV_ABS, ABS_MT_TRACKING_ID, 42, &point);
    send_event(&touch, EV_ABS, ABS_MT_POSITION_X, 600, &point);
    send_event(&touch, EV_ABS, ABS_MT_POSITION_Y, 1200, &point);
    assert(send_event(&touch, EV_SYN, SYN_REPORT, 0, &point) == 0);
    assert(!touch.point.pressed); /* Unknown slot cannot start a touch. */
    send_event(&touch, EV_ABS, ABS_MT_SLOT, 1, &point);
    send_event(&touch, EV_ABS, ABS_MT_TRACKING_ID, 43, &point);
    send_event(&touch, EV_ABS, ABS_MT_POSITION_X, 600, &point);
    send_event(&touch, EV_ABS, ABS_MT_POSITION_Y, 1200, &point);
    assert(send_event(&touch, EV_SYN, SYN_REPORT, 0, &point) == 1);
    assert(point.pressed && touch.primary_slot == 1);
    mock_touch_fd = -1;
    close(pipefd[1]);
    grom_touch_close(&touch);
}

static void type_a(void) {
    grom_touch touch;
    grom_touch_point point = {0};
    init(&touch, GROM_TOUCH_MT_A);
    send_event(&touch, EV_ABS, ABS_MT_POSITION_X, 600, &point);
    send_event(&touch, EV_ABS, ABS_MT_POSITION_Y, 1200, &point);
    send_event(&touch, EV_SYN, SYN_MT_REPORT, 0, &point);
    send_event(&touch, EV_ABS, ABS_MT_POSITION_X, 1000, &point);
    send_event(&touch, EV_ABS, ABS_MT_POSITION_Y, 2000, &point);
    send_event(&touch, EV_SYN, SYN_MT_REPORT, 0, &point);
    assert(send_event(&touch, EV_SYN, SYN_REPORT, 0, &point) == 0);
    assert(!point.pressed && touch.blocked);
    assert(send_event(&touch, EV_SYN, SYN_REPORT, 0, &point) == 0);
    assert(!touch.blocked);
    send_event(&touch, EV_ABS, ABS_MT_POSITION_X, 600, &point);
    send_event(&touch, EV_ABS, ABS_MT_POSITION_Y, 1200, &point);
    assert(send_event(&touch, EV_SYN, SYN_REPORT, 0, &point) == 1);
    assert(point.pressed && point.x == 50 && point.y == 100);
    assert(send_event(&touch, EV_SYN, SYN_REPORT, 0, &point) == 1);
    assert(!point.pressed);
}

/* Himax on the Moto G Fast sends a full point on touch-down followed by
 * pressure-only frames. BTN_TOUCH, not missing coordinates, ends the press. */
static void type_a_sparse_position(void) {
    grom_touch touch;
    grom_touch_point point = {0};
    init(&touch, GROM_TOUCH_MT_A);
    touch.has_btn_touch = true;
    send_event(&touch, EV_ABS, ABS_MT_POSITION_X, 600, &point);
    send_event(&touch, EV_ABS, ABS_MT_POSITION_Y, 1200, &point);
    send_event(&touch, EV_ABS, ABS_MT_TRACKING_ID, 30, &point);
    send_event(&touch, EV_KEY, BTN_TOUCH, 1, &point);
    assert(send_event(&touch, EV_SYN, SYN_REPORT, 0, &point) == 1);
    assert(point.pressed && point.x == 50 && point.y == 100);
    for(int pressure = 39; pressure <= 110; pressure += 7) {
        send_event(&touch, EV_ABS, ABS_MT_PRESSURE, pressure, &point);
        assert(send_event(&touch, EV_SYN, SYN_REPORT, 0, &point) == 0);
        assert(touch.point.pressed && touch.point.x == 50 && touch.point.y == 100);
    }
    send_event(&touch, EV_ABS, ABS_MT_TRACKING_ID, -1, &point);
    send_event(&touch, EV_KEY, BTN_TOUCH, 0, &point);
    assert(send_event(&touch, EV_SYN, SYN_REPORT, 0, &point) == 1);
    assert(!point.pressed);
    send_event(&touch, EV_KEY, BTN_TOUCH, 1, &point);
    assert(send_event(&touch, EV_SYN, SYN_REPORT, 0, &point) == 0);
    assert(!touch.point.pressed); /* No stale position from the prior contact. */
    send_event(&touch, EV_ABS, ABS_MT_POSITION_X, 1000, &point);
    send_event(&touch, EV_ABS, ABS_MT_POSITION_Y, 2000, &point);
    assert(send_event(&touch, EV_SYN, SYN_REPORT, 0, &point) == 1);
    assert(point.pressed && point.x == 90 && point.y == 180);
}

static void single(void) {
    grom_touch touch;
    grom_touch_point point = {0};
    init(&touch, GROM_TOUCH_SINGLE);
    send_event(&touch, EV_ABS, ABS_X, 1200, &point);
    send_event(&touch, EV_ABS, ABS_Y, -10, &point);
    send_event(&touch, EV_KEY, BTN_TOUCH, 1, &point);
    assert(send_event(&touch, EV_SYN, SYN_REPORT, 0, &point) == 1);
    assert(point.pressed && point.x == 100 && point.y == 0);
    send_event(&touch, EV_KEY, BTN_TOUCH, 0, &point);
    assert(send_event(&touch, EV_SYN, SYN_REPORT, 0, &point) == 1);
    assert(!point.pressed && point.x == 100 && point.y == 0);
}

static void disconnect_cancels(void) {
    int pipefd[2];
    assert(pipe(pipefd) == 0);
    grom_touch touch;
    grom_touch_point point = {0};
    init(&touch, GROM_TOUCH_SINGLE);
    touch.fd = pipefd[0];
    struct input_event events[] = {
        {.type = EV_ABS, .code = ABS_X, .value = 600},
        {.type = EV_ABS, .code = ABS_Y, .value = 1200},
        {.type = EV_KEY, .code = BTN_TOUCH, .value = 1},
        {.type = EV_SYN, .code = SYN_REPORT},
    };
    assert(write(pipefd[1], events, sizeof(events)) == (ssize_t)sizeof(events));
    assert(grom_touch_read(&touch, &point) == 1 && point.pressed);
    close(pipefd[1]);
    assert(grom_touch_read(&touch, &point) == 1);
    assert(!point.pressed && touch.fd == -1 && touch.cancel_pending);
}

int main(void) {
    type_b();
    type_b_position_before_id();
    type_b_resync_current_slot();
    type_a();
    type_a_sparse_position();
    single();
    disconnect_cancels();
    puts("touch frames: pass");
    return 0;
}
