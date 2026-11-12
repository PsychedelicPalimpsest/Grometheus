/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GROM_DRIVER_BUNDLE_H
#define GROM_DRIVER_BUNDLE_H

/* Loads the ordered modules in ROOT/modules.load and removes each staged .ko.
 * A missing manifest is a valid device with built-in drivers. */
int grom_drivers_load(const char *root);

/* Injection point for host tests; callback returns 0 or -1 with errno set. */
int grom_drivers_load_with(const char *root, int (*load_module)(int));

#endif
