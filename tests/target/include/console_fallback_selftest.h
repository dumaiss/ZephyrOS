/* ZephyrOS console fallback-matrix selftest contract (test artifact).
 *
 * Shared by every failure-variant image of package 20.  Each image
 * boots the production load sequence (sio.zmod, ioc.zmod, hid.zmod,
 * v9958.zmod from ROMFS, then the single console_bind_defaults()) with
 * one deliberate failure injected through its ROMFS manifest; the
 * variant's exact expected binding is asserted by the host replay
 * (tests/test_console_fallback.py) and by the matching serial_smoke mode,
 * never by this target code.  The target only reports what it observes.
 *
 * Report lines (direct early SIO, independent of the bound console):
 *
 *   console fallback: input <name> class <NN>
 *   console fallback: output <name> class <NN>
 *   console fallback: devices <N>
 *   console fallback: send input          (only when the bound input is the
 *                                     serial fallback: the host may send
 *                                     one byte now)
 *   console fallback: input OK 0xRR       (byte received and echoed back
 *                                     through the same device)
 *   console fallback: input none          (bounded wait expired: no byte)
 *   console fallback: console write OK    (marker written through the bound
 *                                     console output; exact count)
 *   console fallback selftest: OK
 *
 * A direction that cannot be observed through the serial transcript is
 * never claimed as passed here: output-bound directions other than SIO
 * are confirmed by the operator on the V9958 display (or decoded from
 * the V9958 trace by the host replay), and HID-bound input is proven by
 * the full-image human pass, not by this image.
 */

#ifndef ZEPHYROS_TARGET_CONSOLE_FALLBACK_SELFTEST_H
#define ZEPHYROS_TARGET_CONSOLE_FALLBACK_SELFTEST_H

/* Result codes recorded in ace_fallback_result (0 = success). */
#define ZOS_CONSOLEFB_OK            0x00u
#define ZOS_CONSOLEFB_FAIL_BOOT     0x01u /* no console-capable devices        */
#define ZOS_CONSOLEFB_FAIL_CONSOLE  0x02u /* console service wrong             */

/* Recorded outcome for the host replay / smoke transcript. */
extern uint8_t ace_fallback_result;

void zos_run_console_fallback_selftest(void);

#endif /* ZEPHYROS_TARGET_CONSOLE_FALLBACK_SELFTEST_H */
