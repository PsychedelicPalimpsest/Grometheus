#include "grom_preboot.h"

static int (*volatile preboot_entry)(const grom_magisk_boot_config *) = grom_preboot_run;

int main(void) {
    return preboot_entry == 0;
}
