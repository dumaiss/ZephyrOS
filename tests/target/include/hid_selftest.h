/* ZephyrOS HID acceptance selftest contract (test artifact).
 *
 * The kernel-side acceptance test calls zos_run_hid_selftest() from
 * the late boot hook after the production-style boot (SIO + IOC + HID
 * loaded from ROMFS, console bound).  It reports through the direct
 * early SIO for control/outcome lines and through the bound console
 * service for the input/output phases, so the transcript proves HID
 * input reaches the kernel while output stays visible on SIO0/B.
 *
 * Normal image coverage:
 *
 *   - discovers the HID keyboard by class plus capabilities
 *     (ZCAP_HID_TEXT | ZCAP_CHAR_IN | ZCAP_CONSOLE_IN) and rejects any
 *     additional capability bit (gamepad/key-events/output are absent);
 *   - verifies the class interface shape: read_text present,
 *     read_gamepad null, and input-only character operations;
 *   - proves an absent capability is reported as ZDEV_ENOTSUP by a
 *     capability dispatch helper instead of being inferred from the
 *     name;
 *   - proves the console binding policy selected HID input while the
 *     output stayed on the SIO fallback;
 *   - runs the scripted USB-keyboard typing pass (printable keys,
 *     Enter, Backspace, Ctrl-X and one arrow escape sequence) through
 *     the console service and echoes every received byte as an
 *     `hid: rx XX` line on the bound SIO output;
 *   - forces the console input back to the SIO fallback, proving HID is
 *     omitted from the input path; discards any stale host-sync residue
 *     (AAh 55h pairs the smoke tool repeats until the banner is
 *     visible) left in the receiver, and then requires the host to send
 *     one serial byte that must arrive through the same console
 *     service.  Any other byte before the invitation is a failure.
 *
 * No-IOC image coverage (ZOS_HID_NOIOC): HID must be absent after
 * the loader rolled its failed dependency acquisition back; the console
 * must have bound the SIO fallback in both directions, and the serial
 * fallback byte phase runs unchanged.
 *
 * Malformed-IOC image coverage (ZOS_HID_BADIOC): the ROMFS `ioc.zmod`
 * entry carries a fixture that registers an IOC-class device with a null
 * command operation.  HID must find it, reject the interface, roll back,
 * and leave the fixture device and the SIO fallback untouched; the same
 * serial fallback byte phase proves the console still works.
 */

#ifndef ZEPHYROS_TARGET_HID_SELFTEST_H
#define ZEPHYROS_TARGET_HID_SELFTEST_H

/* Result codes recorded in ace_hid_selftest_result (0 = success). */
#define ZOS_HIDSELFTEST_OK                 0x00u
#define ZOS_HIDSELFTEST_FAIL_BOOT          0x01u /* no hid.keyboard0 / not found  */
#define ZOS_HIDSELFTEST_FAIL_SHAPE         0x02u /* caps/interface shape wrong    */
#define ZOS_HIDSELFTEST_FAIL_UNSUPPORTED   0x03u /* ENOTSUP dispatch failed       */
#define ZOS_HIDSELFTEST_FAIL_BIND          0x04u /* console did not prefer HID    */
#define ZOS_HIDSELFTEST_FAIL_TYPED         0x05u /* typed HID byte mismatch/loss  */
#define ZOS_HIDSELFTEST_FAIL_FALLBACK_BIND 0x06u /* forced SIO bind rejected      */
#define ZOS_HIDSELFTEST_FAIL_FALLBACK      0x07u /* SIO fallback byte not 'R'     */
#define ZOS_HIDSELFTEST_FAIL_DEPENDENCY    0x11u /* HID present despite a failed
					     * or malformed IOC dependency  */

/* Recorded outcome for the host replay / smoke transcript. */
extern uint8_t ace_hid_selftest_result;

void zos_run_hid_selftest(void);

#endif /* ZEPHYROS_TARGET_HID_SELFTEST_H */
