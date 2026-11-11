#ifndef GROM_BACKEND_H
#define GROM_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* UI presentation limits, not on-disk geometry. Never truncate matches. */
#define GROM_BACKEND_MAX_MATCHES 128
#define GROM_BACKEND_MAX_PRESERVED 128

typedef uint64_t grom_auth_session;
typedef uint64_t grom_provision_session;
typedef uint64_t grom_slot_choice;

typedef enum {
    GROM_BACKEND_OK,
    GROM_BACKEND_NO_MATCH,
    GROM_BACKEND_INVALID_INPUT,
    GROM_BACKEND_RETRYABLE_ERROR,
    GROM_BACKEND_FATAL_ERROR,
    GROM_BACKEND_UNAVAILABLE
} grom_backend_result;

typedef struct {
    grom_slot_choice choice;
    const char *label;
} grom_match;

typedef struct {
    grom_auth_session session;
    size_t slot_count;
    grom_match slots[GROM_BACKEND_MAX_MATCHES];
    bool administration;
} grom_auth_matches;

typedef struct {
    size_t label_count;
    const char *labels[GROM_BACKEND_MAX_PRESERVED];
    size_t newest_label;
} grom_preservation;

typedef struct {
    bool percent_known;
    uint8_t percent;
} grom_charge_status;

typedef enum {
    GROM_POWER_REBOOT,
    GROM_POWER_SHUTDOWN
} grom_power_action;

/* One backend, with direct calls. The current linked implementation is an
 * inert scaffold. Input secrets are borrowed for one call; the caller erases
 * its copies afterward. Labels are borrowed until the next backend call.
 * Zero is never a valid session ID. Backend owns keys, ranges and sessions. */

/* Evaluate admin and all slots. OK returns only matching labels; NO_MATCH
 * returns none. Never silently truncate a matching set. */
grom_backend_result grom_backend_authenticate(const uint8_t *, size_t,
                                              grom_auth_matches *);
/* Prepare the selected slot for the preboot owner's handoff; on OK erase all
 * unselected secrets and consume the auth session. */
grom_backend_result grom_backend_select_slot(grom_auth_session,
                                             grom_slot_choice);
/* Consume an admin-matching auth session and start provisioning. */
grom_backend_result grom_backend_begin_provisioning(grom_auth_session,
                                                    grom_provision_session *);
void grom_backend_cancel_auth(grom_auth_session);

/* Add authenticated extents to an anonymous in-RAM union. Erase transient
 * secrets and decrypted payloads before returning. */
grom_backend_result grom_backend_preserve(grom_provision_session,
                                          const uint8_t *, size_t);
/* Expose labels authenticated this session, never ranges or inventory. */
grom_backend_result grom_backend_preservation(grom_provision_session,
                                              grom_preservation *);
/* Stage new slot details and credential. Backend validates input and erases
 * staged secrets on replacement, discard or session end. */
grom_backend_result grom_backend_prepare_new_slot(grom_provision_session,
                                                  const char *, uint64_t,
                                                  const uint8_t *, size_t);
void grom_backend_discard_new_slot(grom_provision_session);
/* Backend must require acknowledged_risk even if the UI is bypassed. A
 * successful creation consumes and erases the provisioning session. */
grom_backend_result grom_backend_create_slot(grom_provision_session,
                                             bool acknowledged_risk);
void grom_backend_cancel_provisioning(grom_provision_session);

/* Preboot, not UI, owns charger policy and mode switching. */
grom_backend_result grom_backend_charge_status(grom_charge_status *);
grom_backend_result grom_backend_power(grom_power_action);

#endif
