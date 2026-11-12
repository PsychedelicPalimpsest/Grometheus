#include "grom_backend.h"
#include "power_request.h"

#include <string.h>
#include <unistd.h>

/* All credential and storage operations remain inert. Power requests are
 * forwarded to preboot PID 1; the native preview has no request endpoint. */
static int power_fd = -1;

void grom_backend_set_power_fd(int fd) {
    power_fd = fd;
}
grom_backend_result grom_backend_authenticate(const uint8_t *secret,
                                              size_t length,
                                              grom_auth_matches *out) {
    (void)secret; (void)length;
    memset(out, 0, sizeof(*out));
    return GROM_BACKEND_NO_MATCH;
}

grom_backend_result grom_backend_select_slot(grom_auth_session session,
                                             grom_slot_choice choice) {
    (void)session; (void)choice;
    return GROM_BACKEND_UNAVAILABLE;
}

grom_backend_result grom_backend_begin_provisioning(grom_auth_session auth,
                                                    grom_provision_session *out) {
    (void)auth;
    *out = 0;
    return GROM_BACKEND_UNAVAILABLE;
}

void grom_backend_cancel_auth(grom_auth_session session) {
    (void)session;
}

grom_backend_result grom_backend_preserve(grom_provision_session session,
                                          const uint8_t *secret, size_t length) {
    (void)session; (void)secret; (void)length;
    return GROM_BACKEND_UNAVAILABLE;
}

grom_backend_result grom_backend_preservation(grom_provision_session session,
                                              grom_preservation *out) {
    (void)session;
    memset(out, 0, sizeof(*out));
    return GROM_BACKEND_UNAVAILABLE;
}

grom_backend_result grom_backend_prepare_new_slot(grom_provision_session session,
                                                  const char *label, uint64_t size,
                                                  const uint8_t *secret,
                                                  size_t length) {
    (void)session; (void)label; (void)size; (void)secret; (void)length;
    return GROM_BACKEND_UNAVAILABLE;
}

void grom_backend_discard_new_slot(grom_provision_session session) {
    (void)session;
}

grom_backend_result grom_backend_create_slot(grom_provision_session session,
                                             bool acknowledged) {
    (void)session; (void)acknowledged;
    return GROM_BACKEND_UNAVAILABLE;
}

void grom_backend_cancel_provisioning(grom_provision_session session) {
    (void)session;
}

grom_backend_result grom_backend_charge_status(grom_charge_status *out) {
    memset(out, 0, sizeof(*out));
    return GROM_BACKEND_UNAVAILABLE;
}

grom_backend_result grom_backend_power(grom_power_action action) {
    if(action != GROM_POWER_REBOOT && action != GROM_POWER_SHUTDOWN)
        return GROM_BACKEND_INVALID_INPUT;
    if(power_fd < 0) return GROM_BACKEND_UNAVAILABLE;
    char request = action == GROM_POWER_SHUTDOWN ? 'S' : 'R';
    return write(power_fd, &request, 1) == 1 ? GROM_BACKEND_OK :
                                               GROM_BACKEND_RETRYABLE_ERROR;
}
