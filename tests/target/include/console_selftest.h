/* ZephyrOS console acceptance contract (test artifact).
 *
 * The kernel-side diagnostic runs from the late boot hook
 * (zos_run_console_selftest) after the resident service loader started
 * the store's services and the nucleus console probe bound the console
 * endpoint.  It reports the composition the console service derived
 * (input class 04/01, output class 02/01), writes the visible marker
 * `m8`, reads the typed line the replay queues on the input lane, and
 * hands the image to the unchanged M7 monitor launcher.  The transcript
 * goes through the direct early SIO.
 */

#ifndef ZEPHYROS_TARGET_CONSOLE_SELFTEST_H
#define ZEPHYROS_TARGET_CONSOLE_SELFTEST_H

/* Recorded outcome for the host replay / smoke transcript (0 = success). */
extern uint8_t ace_console_result;

void zos_run_console_selftest(void);

#endif /* ZEPHYROS_TARGET_CONSOLE_SELFTEST_H */
