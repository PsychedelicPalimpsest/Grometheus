#ifndef GROM_PREBOOT_H
#define GROM_PREBOOT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Called only by first-stage MagiskInit as PID 1. Return 0 only after a slot's
 * storage setup is complete. Any other result must halt before stock init. */
int grom_preboot_run(void);

#ifdef __cplusplus
}
#endif

#endif
