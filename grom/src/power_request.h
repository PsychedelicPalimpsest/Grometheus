#ifndef GROM_POWER_REQUEST_H
#define GROM_POWER_REQUEST_H

/* Renderer-only endpoint of the power request pipe owned by preboot PID 1. */
void grom_backend_set_power_fd(int fd);

#endif
