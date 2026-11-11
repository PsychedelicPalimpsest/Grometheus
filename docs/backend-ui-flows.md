# Backend calls from the preboot UI

This page maps the interface in [`grom/include/grom_backend.h`](../grom/include/grom_backend.h)
to calls made by [`grom/ui/outline.c`](../grom/ui/outline.c). It describes the
current frontend and the intended backend responsibilities. The combined
renderer currently links [`grom/src/backend_stub.c`](../grom/src/backend_stub.c),
which never unlocks a slot or performs a privileged action.

## Mode entry and lifetime

Preboot creates the frontend with `grom_ui_create(display)` and chooses one of
two mode entry calls. Creation and destruction only manage the frontend's
lifetime:

| UI call | Backend calls | Current effect |
| --- | --- | --- |
| `grom_ui_show_password(ui)` | `grom_backend_cancel_auth` or `grom_backend_cancel_provisioning` if a session is live | Clears prior session data and shows the shared password prompt. Authentication waits for a nonempty credential submission. |
| `grom_ui_show_charging(ui)` | The same session cleanup, then `grom_backend_charge_status` immediately and every five seconds | Shows the charging screen. An unavailable or invalid percentage is omitted. Preboot owns charger policy and mode switching. |
| `grom_ui_destroy(ui)` | `grom_backend_cancel_auth` or `grom_backend_cancel_provisioning` if a session is live | Stops charge polling and clears frontend data. |

The combined preboot scaffold currently calls only `grom_ui_show_password`.
It has no input device hookup, so its displayed password field cannot submit a
credential. The charging entry call is present but no charger policy selects it.
The SDL preview accepts UI interaction but uses the same inert backend stub.

## Authentication and destination selection

| User action or outcome | Backend call | UI behavior |
| --- | --- | --- |
| Submit a nonempty password or PIN | `grom_backend_authenticate(credential, length, &matches)` | Erases its input buffer after the call. The future backend must check both the admin verifier and all slots before returning. |
| `GROM_BACKEND_NO_MATCH` | None further, except `grom_backend_cancel_auth` if a session was unexpectedly returned | Shows one generic unlock error and no labels. |
| Authentication error or malformed result | `grom_backend_cancel_auth` if a session was returned | Stays in preboot with retry and shutdown choices. |
| Exactly one slot match, no admin match | `grom_backend_select_slot(session, choice)` automatically | On success, shows “Starting slot…”; the preboot owner must still implement Android handoff. On failure, calls `grom_backend_cancel_auth` and shows a boot error. |
| Admin match only | `grom_backend_begin_provisioning(session, &provision_session)` automatically | On success, enters the preservation flow. On failure, calls `grom_backend_cancel_auth` and stays in preboot. |
| Multiple matching destinations, including one slot plus Administration | No selection call yet | Shows only matching slot labels and Administration if matched. A chosen slot calls `grom_backend_select_slot`; Administration calls `grom_backend_begin_provisioning`. |
| Cancel selection, switch entry mode, or destroy the UI | `grom_backend_cancel_auth(session)` | Erases pending choices and visible labels. |

The auth session and slot choices are opaque. The future backend owns matching
secrets. It must consume the session and erase unselected secrets on successful
slot selection. The UI copies only returned display labels, clears those copies
and LVGL text when leaving selection, and never receives an allocation map or
global slot inventory.

## Provisioning

This flow is reachable only after `grom_backend_begin_provisioning` succeeds.
The backend must own the in-RAM occupied-range union, preservation labels and
staged new-slot secret. The UI displays permitted labels and collects inputs.

| User action or outcome | Backend call | UI behavior |
| --- | --- | --- |
| Add an existing slot credential | `grom_backend_preserve(provision_session, credential, length)` | Erases the input buffer after the call. On no match, shows a generic error and stays at the prompt. |
| Successful preservation | `grom_backend_preservation(provision_session, &list)` | Copies only labels authenticated in this provisioning session into the on-screen preservation list. No ranges are returned. |
| Preservation or label-query error | `grom_backend_cancel_provisioning(session)` | Erases the UI's copied labels and shows a provisioning failure. |
| Continue to new-slot details | None | Moves to the label and size form. The UI parses a positive GiB value; the backend must validate the proposed allocation later. |
| Submit and confirm a new slot password | `grom_backend_prepare_new_slot(session, label, size_gib, credential, length)` after a local equality check | Erases both UI password buffers. On success, shows risk review. The backend must own any staged secret. |
| Back from risk review | `grom_backend_discard_new_slot(session)` | Clears the acknowledgement and returns to the credential form. |
| Check the overwrite acknowledgement | None | Updates a local control. It does not authorize creation in the backend. |
| Create after acknowledgement | `grom_backend_create_slot(session, true)` | The backend must enforce acknowledgement and safe allocation. On success, the session is consumed and the UI offers reboot only. On failure, the UI calls `grom_backend_cancel_provisioning` and stays in preboot. |
| Cancel, switch entry mode, or destroy the UI | `grom_backend_cancel_provisioning(session)` | The backend must erase accumulated ranges, staged secrets and retained labels; the UI clears its copies. |

Back navigation between preservation, new-slot details and new-password forms
is local to the UI, except that leaving risk review discards the staged draft.
Initial admin PIN setup and external PIN replacement are outside this preboot
UI interface.

## Power and errors

“Reboot” after creation calls `grom_backend_power(GROM_POWER_REBOOT)`. A visible
“Shut down” action on failure pages calls
`grom_backend_power(GROM_POWER_SHUTDOWN)`. If power is unavailable, the UI stays
in preboot. Retry cancels any live session and returns to the password prompt.

Calls are synchronous and direct. `GROM_BACKEND_NO_MATCH` is a generic
credential failure. `GROM_BACKEND_INVALID_INPUT`, `GROM_BACKEND_RETRYABLE_ERROR`,
`GROM_BACKEND_FATAL_ERROR` and `GROM_BACKEND_UNAVAILABLE` are distinct API
results, but the current frontend does not display their internals. The linked
stub returns `GROM_BACKEND_NO_MATCH` for authentication,
`GROM_BACKEND_UNAVAILABLE` for other active operations, and unknown charge
status. Its cancellation functions are no-ops because it creates no sessions.
