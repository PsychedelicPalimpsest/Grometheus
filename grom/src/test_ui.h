#ifndef GROM_TEST_UI_H
#define GROM_TEST_UI_H

/* Runs only in the supervised renderer child. Writes one byte to ready_fd
 * after the first successful presentation. */
int grom_test_ui_run(int ready_fd);

#endif
