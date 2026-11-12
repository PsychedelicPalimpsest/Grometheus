/* SPDX-License-Identifier: Apache-2.0 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "touch.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define BITS_PER_LONG (sizeof(unsigned long) * CHAR_BIT)
#define BIT_WORDS(last) ((last) / BITS_PER_LONG + 1)

static bool kernel_all_up(grom_touch *touch);

static void refresh_current_slot(grom_touch *touch) {
    struct input_absinfo slot;
    if(ioctl(touch->fd, EVIOCGABS(ABS_MT_SLOT), &slot) >= 0 &&
       slot.value >= 0 && (unsigned)slot.value < touch->slot_count)
        touch->current_slot = slot.value;
    else
        touch->current_slot = -1; /* Wait for an explicit ABS_MT_SLOT. */
}

static bool has_bit(const unsigned long *bits, unsigned code) {
    return (bits[code / BITS_PER_LONG] & (1UL << (code % BITS_PER_LONG))) != 0;
}

void grom_touch_init(grom_touch *touch, uint32_t width, uint32_t height) {
    memset(touch, 0, sizeof(*touch));
    touch->fd = -1;
    touch->width = width;
    touch->height = height;
    touch->primary_slot = -1;
    touch->blocked = true; /* An already-held finger cannot click on attach. */
    touch->resync_required = true;
}

void grom_touch_close(grom_touch *touch) {
    if(touch->fd >= 0) close(touch->fd);
    uint32_t width = touch->width, height = touch->height;
    grom_touch_init(touch, width, height);
}

static int inspect(int fd, grom_touch *touch) {
    unsigned long abs_bits[BIT_WORDS(ABS_MAX)] = {0};
    unsigned long key_bits[BIT_WORDS(KEY_MAX)] = {0};
    unsigned long props[BIT_WORDS(INPUT_PROP_MAX)] = {0};
    if(ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0) return 0;
    if(ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) return 0;
    if(ioctl(fd, EVIOCGPROP(sizeof(props)), props) < 0) memset(props, 0, sizeof(props));
    if(has_bit(props, INPUT_PROP_POINTER)) return 0;

    bool mt = has_bit(abs_bits, ABS_MT_POSITION_X) && has_bit(abs_bits, ABS_MT_POSITION_Y);
    bool single = has_bit(abs_bits, ABS_X) && has_bit(abs_bits, ABS_Y) &&
                  has_bit(key_bits, BTN_TOUCH);
    touch->has_btn_touch = has_bit(key_bits, BTN_TOUCH);
    if(has_bit(key_bits, BTN_GAMEPAD)) return 0;
    if(mt && has_bit(abs_bits, ABS_MT_SLOT) && has_bit(abs_bits, ABS_MT_TRACKING_ID)) {
        touch->protocol = GROM_TOUCH_MT_B;
        struct input_absinfo slot;
        if(ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &slot) < 0 || slot.minimum != 0 ||
           slot.maximum < 0 || slot.maximum >= GROM_TOUCH_SLOTS) return 0;
        touch->slot_count = (unsigned)slot.maximum + 1;
        touch->current_slot = slot.value >= 0 &&
                              (unsigned)slot.value < touch->slot_count ?
                              slot.value : -1;
    } else if(mt) {
        touch->protocol = GROM_TOUCH_MT_A;
    } else if(single) {
        touch->protocol = GROM_TOUCH_SINGLE;
    } else {
        return 0;
    }
    unsigned xcode = mt ? ABS_MT_POSITION_X : ABS_X;
    unsigned ycode = mt ? ABS_MT_POSITION_Y : ABS_Y;
    if(ioctl(fd, EVIOCGABS(xcode), &touch->axis_x) < 0 ||
       ioctl(fd, EVIOCGABS(ycode), &touch->axis_y) < 0 ||
       touch->axis_x.maximum <= touch->axis_x.minimum ||
       touch->axis_y.maximum <= touch->axis_y.minimum) return 0;
    return has_bit(props, INPUT_PROP_DIRECT) ? 2 : 1;
}

int grom_touch_discover(grom_touch *touch) {
    if(touch->fd >= 0) return 0;
    DIR *dir = opendir("/dev/input");
    if(!dir) return -1;
    int best_fd = -1, best_rank = 0;
    grom_touch best = *touch;
    struct dirent *entry;
    while((entry = readdir(dir))) {
        const char *name = entry->d_name;
        if(memcmp(name, "event", 5) || name[5] < '0' || name[5] > '9') continue;
        bool numeric = true;
        for(const char *p = name + 5; *p; ++p) {
            if(*p < '0' || *p > '9') { numeric = false; break; }
        }
        if(!numeric) continue;
        char path[256];
        int size = snprintf(path, sizeof(path), "/dev/input/%s", name);
        if(size < 0 || (size_t)size >= sizeof(path)) continue;
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if(fd < 0) continue;
        grom_touch candidate = *touch;
        int rank = inspect(fd, &candidate);
        if(rank > best_rank) {
            if(best_fd >= 0) close(best_fd);
            best_fd = fd;
            best_rank = rank;
            best = candidate;
        } else {
            close(fd);
        }
    }
    closedir(dir);
    if(best_fd < 0) return -1;
    best.fd = best_fd;
    best.blocked = !kernel_all_up(&best);
    best.resync_required = best.blocked;
    *touch = best;
    return 0;
}

static int32_t scale(int32_t raw, struct input_absinfo axis, uint32_t size) {
    if(size <= 1) return 0;
    if(raw < axis.minimum) raw = axis.minimum;
    if(raw > axis.maximum) raw = axis.maximum;
    return (int32_t)(((int64_t)raw - axis.minimum) * (size - 1) /
                     ((int64_t)axis.maximum - axis.minimum));
}

static int emit(grom_touch *touch, bool pressed, int32_t x, int32_t y,
                grom_touch_point *point) {
    if(pressed) {
        x = scale(x, touch->axis_x, touch->width);
        y = scale(y, touch->axis_y, touch->height);
    } else {
        x = touch->point.x;
        y = touch->point.y;
    }
    bool changed = pressed != touch->point.pressed ||
                   (pressed && (x != touch->point.x || y != touch->point.y));
    touch->point = (grom_touch_point){.x = x, .y = y, .pressed = pressed};
    *point = touch->point;
    return changed ? 1 : 0;
}

static bool all_slots_up(const grom_touch *touch) {
    for(unsigned i = 0; i < GROM_TOUCH_SLOTS; ++i) {
        if(touch->slots[i].active) return false;
    }
    return true;
}

static unsigned active_slots(const grom_touch *touch) {
    unsigned count = 0;
    for(unsigned i = 0; i < GROM_TOUCH_SLOTS; ++i) {
        if(touch->slots[i].active) ++count;
    }
    return count;
}

static bool kernel_all_up(grom_touch *touch) {
    if(touch->fd < 0) return false;
    if(touch->protocol == GROM_TOUCH_MT_B) {
        int32_t ids[GROM_TOUCH_SLOTS + 1] = {0};
        ids[0] = ABS_MT_TRACKING_ID;
        size_t bytes = (touch->slot_count + 1) * sizeof(ids[0]);
        if(ioctl(touch->fd, EVIOCGMTSLOTS(bytes), ids) < 0) return false;
        for(unsigned i = 1; i <= touch->slot_count; ++i) {
            if(ids[i] >= 0) return false;
        }
        return true;
    }
    if(touch->protocol == GROM_TOUCH_SINGLE) {
        unsigned long keys[BIT_WORDS(KEY_MAX)] = {0};
        return ioctl(touch->fd, EVIOCGKEY(sizeof(keys)), keys) >= 0 &&
               !has_bit(keys, BTN_TOUCH);
    }
    if(touch->protocol == GROM_TOUCH_MT_A && touch->has_btn_touch) {
        unsigned long keys[BIT_WORDS(KEY_MAX)] = {0};
        return ioctl(touch->fd, EVIOCGKEY(sizeof(keys)), keys) >= 0 &&
               !has_bit(keys, BTN_TOUCH);
    }
    return !touch->any_packet;
}

static int frame(grom_touch *touch, grom_touch_point *point) {
    if(touch->protocol == GROM_TOUCH_MT_A &&
       touch->packet.has_x && touch->packet.has_y && !touch->any_packet) {
        touch->first_packet = touch->packet;
        touch->any_packet = true;
        ++touch->packet_count;
    }
    if(touch->blocked) {
        bool up = false;
        if(touch->resync_required) up = kernel_all_up(touch);
        else if(touch->protocol == GROM_TOUCH_MT_B) up = all_slots_up(touch);
        else if(touch->protocol == GROM_TOUCH_MT_A)
            up = touch->has_btn_touch ? !touch->single_down : !touch->any_packet;
        touch->packet = (grom_touch_contact){0};
        touch->any_packet = false;
        touch->packet_count = 0;
        if(up) {
            touch->blocked = false;
            touch->resync_required = false;
            memset(touch->slots, 0, sizeof(touch->slots));
            touch->single_down = false;
        }
        return emit(touch, false, 0, 0, point);
    }
    if(touch->protocol == GROM_TOUCH_MT_B) {
        unsigned count = active_slots(touch);
        if(touch->primary_slot >= 0) {
            grom_touch_contact *primary = &touch->slots[touch->primary_slot];
            if(!primary->active || primary->tracking_id != touch->primary_id || count != 1) {
                if(count > 1 || (primary->active && primary->tracking_id != touch->primary_id))
                    touch->cancel_pending = true;
                touch->primary_slot = -1;
                touch->blocked = count != 0;
                touch->resync_required = false;
                return emit(touch, false, 0, 0, point);
            }
            if(primary->has_x && primary->has_y)
                return emit(touch, true, primary->x, primary->y, point);
        } else if(count > 1) {
            touch->blocked = true;
            touch->resync_required = false;
        } else {
            for(unsigned i = 0; i < GROM_TOUCH_SLOTS; ++i) {
                grom_touch_contact *contact = &touch->slots[i];
                if(contact->active && contact->has_x && contact->has_y) {
                    touch->primary_slot = (int)i;
                    touch->primary_id = contact->tracking_id;
                    return emit(touch, true, contact->x, contact->y, point);
                }
            }
        }
    } else if(touch->protocol == GROM_TOUCH_MT_A) {
        if(touch->packet_count > 1) {
            touch->blocked = true;
            touch->resync_required = false;
            touch->cancel_pending = touch->point.pressed;
            touch->packet = (grom_touch_contact){0};
            touch->any_packet = false;
            touch->packet_count = 0;
            return emit(touch, false, 0, 0, point);
        }
        /* Some protocol-A drivers report the position only on touch-down and
         * send pressure-only SYN_REPORT frames until BTN_TOUCH goes up. Keep
         * the last point in that case; a frame without coordinates is not a
         * release while the kernel still reports the finger down. */
        bool active = touch->has_btn_touch ? touch->single_down : touch->any_packet;
        int32_t x = touch->first_packet.x, y = touch->first_packet.y;
        touch->packet = (grom_touch_contact){0};
        touch->any_packet = false;
        touch->packet_count = 0;
        if(!active) touch->first_packet = (grom_touch_contact){0};
        if(active && !touch->first_packet.has_x) return 0;
        if(active && !touch->first_packet.has_y) return 0;
        return emit(touch, active, x, y, point);
    } else if(touch->protocol == GROM_TOUCH_SINGLE) {
        return emit(touch, touch->single_down, touch->single_x, touch->single_y, point);
    }
    return 0;
}

int grom_touch_feed(grom_touch *touch, const struct input_event *event,
                    grom_touch_point *point) {
    if(event->type == EV_SYN && event->code == SYN_DROPPED) {
        ++touch->dropped_frames;
        touch->dropping = true;
        touch->blocked = true;
        touch->resync_required = true;
        touch->cancel_pending = true;
        touch->primary_slot = -1;
        touch->packet = (grom_touch_contact){0};
        touch->first_packet = (grom_touch_contact){0};
        touch->any_packet = false;
        touch->packet_count = 0;
        touch->single_down = false;
        memset(touch->slots, 0, sizeof(touch->slots));
        return emit(touch, false, 0, 0, point);
    }
    if(touch->dropping) {
        if(event->type == EV_SYN && event->code == SYN_REPORT) {
            touch->dropping = false;
            if(kernel_all_up(touch)) {
                if(touch->protocol == GROM_TOUCH_MT_B)
                    refresh_current_slot(touch);
                touch->blocked = false;
                touch->resync_required = false;
                memset(touch->slots, 0, sizeof(touch->slots));
                touch->single_down = false;
            }
        }
        return 0;
    }
    if(event->type == EV_SYN) {
        if(event->code == SYN_MT_REPORT && touch->protocol == GROM_TOUCH_MT_A) {
            if(touch->packet.has_x && touch->packet.has_y && !touch->any_packet) {
                touch->first_packet = touch->packet;
                touch->any_packet = true;
            }
            if(touch->packet.has_x && touch->packet.has_y && touch->packet_count < 2)
                ++touch->packet_count;
            touch->packet = (grom_touch_contact){0};
        }
        if(event->code == SYN_REPORT) {
            int result = frame(touch, point);
            if(touch->protocol == GROM_TOUCH_MT_B) {
                for(unsigned i = 0; i < touch->slot_count; ++i) {
                    touch->slots[i].x_this_frame = false;
                    touch->slots[i].y_this_frame = false;
                }
            }
            return result;
        }
        return 0;
    }
    if(event->type == EV_KEY && event->code == BTN_TOUCH &&
       (touch->protocol == GROM_TOUCH_SINGLE ||
        touch->protocol == GROM_TOUCH_MT_A)) {
        touch->single_down = event->value != 0;
    }
    if(event->type != EV_ABS) return 0;
    if(touch->protocol == GROM_TOUCH_MT_B) {
        if(event->code == ABS_MT_SLOT) {
            touch->current_slot = event->value;
        } else if(touch->current_slot >= 0 &&
                  (unsigned)touch->current_slot < touch->slot_count) {
            grom_touch_contact *contact = &touch->slots[touch->current_slot];
            if(event->code == ABS_MT_TRACKING_ID) {
                if(event->value < 0) *contact = (grom_touch_contact){0};
                else {
                    /* Some MT-B drivers send the new point before its ID.
                     * Keep coordinates reported in this frame, while dropping
                     * any coordinates left by the preceding contact. */
                    int32_t x = contact->x, y = contact->y;
                    bool x_new = contact->x_this_frame;
                    bool y_new = contact->y_this_frame;
                    *contact = (grom_touch_contact){0};
                    contact->active = true;
                    contact->tracking_id = event->value;
                    if(x_new) {
                        contact->x = x;
                        contact->has_x = true;
                        contact->x_this_frame = true;
                    }
                    if(y_new) {
                        contact->y = y;
                        contact->has_y = true;
                        contact->y_this_frame = true;
                    }
                }
            } else if(event->code == ABS_MT_POSITION_X) {
                contact->x = event->value;
                contact->has_x = true;
                contact->x_this_frame = true;
            } else if(event->code == ABS_MT_POSITION_Y) {
                contact->y = event->value;
                contact->has_y = true;
                contact->y_this_frame = true;
            }
        }
    } else if(touch->protocol == GROM_TOUCH_MT_A) {
        if(event->code == ABS_MT_POSITION_X) {
            touch->packet.x = event->value;
            touch->packet.has_x = true;
        } else if(event->code == ABS_MT_POSITION_Y) {
            touch->packet.y = event->value;
            touch->packet.has_y = true;
        }
    } else if(touch->protocol == GROM_TOUCH_SINGLE) {
        if(event->code == ABS_X) touch->single_x = event->value;
        if(event->code == ABS_Y) touch->single_y = event->value;
    }
    return 0;
}

int grom_touch_read(grom_touch *touch, grom_touch_point *point) {
    if(touch->fd < 0) return 0;
    struct input_event event;
    for(;;) {
        ssize_t count = read(touch->fd, &event, sizeof(event));
        if(count == (ssize_t)sizeof(event)) {
            int result = grom_touch_feed(touch, &event, point);
            if(result) {
                touch->last_event_ms = (uint32_t)
                    ((uint64_t)event.time.tv_sec * 1000u +
                     (uint64_t)event.time.tv_usec / 1000u);
                return result;
            }
        } else if(count < 0 && errno == EINTR) {
            continue;
        } else if(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        } else {
            bool pressed = touch->point.pressed;
            grom_touch_point last = touch->point;
            grom_touch_close(touch);
            touch->point.x = last.x;
            touch->point.y = last.y;
            touch->cancel_pending = pressed;
            *point = touch->point;
            return pressed ? 1 : -1;
        }
    }
}
