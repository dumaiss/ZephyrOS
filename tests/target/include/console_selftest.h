/* ZephyrOS console acceptance selftest contract (test artifact).
 *
 * The kernel-side acceptance test calls zos_run_console_selftest()
 * from the late boot hook after the production-style boot (SIO + IOC +
 * HID + the V9958 video core with its output terminal renderer loaded
 * from ROMFS) and after the console service has bound the default
 * HID-input / V9958-output pair.  Control and outcome lines go through
 * the direct early SIO, independent of the bound console, so the serial
 * transcript is the pass/fail record; the renderer's visible output is
 * checked by the host replay and by the operator.
 *
 * Coverage:
 *
 *   - reports the selected input and output classes/instances on the
 *     early SIO (`hid/keyboard0 class 04`, `v9958.0 class 02`);
 *   - verifies the output device carries exactly the seven VIDEO
 *     capabilities plus ZCAP_CHAR_OUT | ZCAP_CONSOLE_OUT and an
 *     output-only zchar_ops table (putc/write present, poll/getc null);
 *   - verifies the default binding is exactly the discovered HID input
 *     and V9958 output handles;
 *   - proves the console service drives the renderer: console_write
 *     returns the exact transfer count for printable plus CR/LF text,
 *     console_putc accepts single bytes, and an SGR reverse sequence is
 *     consumed;
 *   - injects VIDEO-operation errors (bad PRESENT length, unknown
 *     operation, null state) with the exact error codes and proves the
 *     console direction and the whole registry still work, including a
 *     full ZVIDEO_RESET followed by more visible output.
 */

#ifndef ZEPHYROS_TARGET_CONSOLE_SELFTEST_H
#define ZEPHYROS_TARGET_CONSOLE_SELFTEST_H

/* Result codes recorded in ace_console_selftest_result (0 = success). */
#define ZOS_CONSOLESELFTEST_OK             0x00u
#define ZOS_CONSOLESELFTEST_FAIL_BOOT      0x01u /* devices not discovered      */
#define ZOS_CONSOLESELFTEST_FAIL_SHAPE     0x02u /* caps/interface shape wrong  */
#define ZOS_CONSOLESELFTEST_FAIL_BIND      0x03u /* default binding wrong       */
#define ZOS_CONSOLESELFTEST_FAIL_OPS       0x04u /* console/render op wrong     */
#define ZOS_CONSOLESELFTEST_FAIL_DEVICES   0x05u /* registry disturbed          */

/* Recorded outcome for the host replay / smoke transcript. */
extern uint8_t ace_console_selftest_result;

void zos_run_console_selftest(void);

#endif /* ZEPHYROS_TARGET_CONSOLE_SELFTEST_H */
