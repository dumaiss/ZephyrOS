/* ZephyrOS M8 V9958 video acceptance selftest contract (test artifact).
 *
 * The kernel-side acceptance test calls zos_run_m8_v9958_selftest()
 * from the late boot hook after the production-style boot (SIO + IOC +
 * HID + the V9958 video core loaded from ROMFS).  Control and outcome
 * lines go through the direct early SIO, independent of every bound
 * console, so the transcript is the pass/fail record.
 *
 * Normal image coverage:
 *
 *   - discovers `v9958.0` by class (ZDEV_VIDEO) with exactly the seven
 *     VIDEO bits plus ZCAP_CHAR_OUT | ZCAP_CONSOLE_OUT (the ported
 *     output terminal renderer) and an output-only zchar_ops table
 *     (putc/write present, poll/getc null);
 *   - verifies the class interface through device_interface();
 *   - proves malformed requests are rejected before any I/O
 *     (non-null/nonzero write validation, zero-length PRESENT/RESET,
 *     unknown operation -> ZDEV_ENOTSUP, null state -> ZDEV_ENODEV);
 *   - verifies the default console binding is the normal M8 pair: the
 *     HID keyboard input and the v9958.0 output;
 *   - runs ZVIDEO_RESET and re-establishes the full display state;
 *   - exercises the native operations end to end: CTRL_WRITE register
 *     selects, PALETTE_WRITE for one palette entry, INDIRECT_WRITE for
 *     a HMMV fill command, PRESENT for command completion, and
 *     DATA_WRITE + DATA_BLOCK (200-byte payload, far above the old
 *     16-byte ceiling) for a deterministic visible pattern.
 *
 * The visible result is documented in docs/validation/: a white filled
 * rectangle, a magenta palette entry, and a white-then-magenta band.
 * tests/test_v9958_sim.py replays the image against the V9958 peer and
 * checks the exact VRAM bytes.
 *
 * Font-failure image coverage (ZOS_M8_V9958_FONTFAIL): the ROMFS
 * `v9958-6x8.font` entry is an undersized stub, so the module init must
 * fail its resource check and publish no device.  The image proves
 * `v9958.0` is absent, HID and SIO are still registered, and the
 * console service still binds (SIO output); V9958-specific operations
 * are skipped.
 */

#ifndef ZEPHYROS_TARGET_M8_V9958_SELFTEST_H
#define ZEPHYROS_TARGET_M8_V9958_SELFTEST_H

/* Result codes recorded in m8_v9958_selftest_result (0 = success). */
#define ZOS_M8V9958_OK              0x00u
#define ZOS_M8V9958_FAIL_BOOT       0x01u /* no v9958.0 / not found       */
#define ZOS_M8V9958_FAIL_SHAPE      0x02u /* caps/interface shape wrong   */
#define ZOS_M8V9958_FAIL_MALFORMED  0x03u /* malformed request not rejected */
#define ZOS_M8V9958_FAIL_OPS        0x04u /* an operation returned nonzero  */
#define ZOS_M8V9958_FAIL_BIND       0x05u /* console binding wrong         */
#define ZOS_M8V9958_FAIL_PRESENT    0x11u /* device present despite the
					     * failed font resource         */
#define ZOS_M8V9958_FAIL_DEVICES    0x12u /* earlier devices disturbed    */

/* Recorded outcome for the host replay / smoke transcript. */
extern uint8_t m8_v9958_selftest_result;

void zos_run_m8_v9958_selftest(void);

#endif /* ZEPHYROS_TARGET_M8_V9958_SELFTEST_H */
