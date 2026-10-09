/* ZephyrOS console fallback acceptance contract (test artifact).
 *
 * The kernel-side diagnostic runs from the late boot hook
 * (zos_run_console_fallback_selftest) on images whose store lists a
 * *subset* of the resident-service set.  It reports the composition the
 * console service derived from the registered endpoints, writes one
 * marker through the console output and reads one typed line through the
 * console input, so the replay can pin which lane each direction took:
 *
 *   nohid          output video, input serial;
 *   nohid-nov9958  output serial, input serial.
 *
 * The transcript goes through the direct early SIO.
 */

#ifndef ZEPHYROS_TARGET_CONSOLE_FALLBACK_SELFTEST_H
#define ZEPHYROS_TARGET_CONSOLE_FALLBACK_SELFTEST_H

/* Recorded outcome for the host replay / smoke transcript (0 = success). */
extern uint8_t ace_fallback_result;

void zos_run_console_fallback_selftest(void);

#endif /* ZEPHYROS_TARGET_CONSOLE_FALLBACK_SELFTEST_H */
