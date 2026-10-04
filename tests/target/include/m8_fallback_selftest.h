/* ZephyrOS M8 fallback-matrix selftest contract (test artifact).
 *
 * Shared by every failure-variant image of package 20.  Each image
 * boots the production load sequence (sio.zmod, ioc.zmod, hid.zmod,
 * v9958.zmod from ROMFS, then the single console_bind_defaults()) with
 * one deliberate failure injected through its ROMFS manifest; the
 * variant's exact expected binding is asserted by the host replay
 * (tests/test_m8_fallback.py) and by the matching serial_smoke mode,
 * never by this target code.  The target only reports what it observes.
 *
 * Report lines (direct early SIO, independent of the bound console):
 *
 *   m8 fallback: input <name> class <NN>
 *   m8 fallback: output <name> class <NN>
 *   m8 fallback: devices <N>
 *   m8 fallback: send input          (only when the bound input is the
 *                                     serial fallback: the host may send
 *                                     one byte now)
 *   m8 fallback: input OK 0xRR       (byte received and echoed back
 *                                     through the same device)
 *   m8 fallback: input none          (bounded wait expired: no byte)
 *   m8 fallback: console write OK    (marker written through the bound
 *                                     console output; exact count)
 *   m8 fallback selftest: OK
 *
 * A direction that cannot be observed through the serial transcript is
 * never claimed as passed here: output-bound directions other than SIO
 * are confirmed by the operator on the V9958 display (or decoded from
 * the V9958 trace by the host replay), and HID-bound input is proven by
 * the full-image human pass, not by this image.
 */

#ifndef ZEPHYROS_TARGET_M8_FALLBACK_SELFTEST_H
#define ZEPHYROS_TARGET_M8_FALLBACK_SELFTEST_H

/* Result codes recorded in m8_fallback_selftest_result (0 = success). */
#define ZOS_M8FB_OK            0x00u
#define ZOS_M8FB_FAIL_BOOT     0x01u /* no console-capable devices        */
#define ZOS_M8FB_FAIL_CONSOLE  0x02u /* console service wrong             */

/* Recorded outcome for the host replay / smoke transcript. */
extern uint8_t m8_fallback_selftest_result;

void zos_run_m8_fallback_selftest(void);

#endif /* ZEPHYROS_TARGET_M8_FALLBACK_SELFTEST_H */
