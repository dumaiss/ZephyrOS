/* ZephyrOS console integration acceptance contract (test artifact).
 *
 * The kernel-side integration diagnostic runs from the late boot hook
 * with zos_run_console_integration_selftest(): by then the resident
 * driver loader has placed and started every driver the store's
 * manifest lists and the nucleus console probe has bound the console
 * endpoint, so the diagnostic is a bank-7 client of the console, IOC,
 * HID, video and serial drivers (tests/target/include/driver_client.h).
 *
 * Control and outcome lines go through the direct early SIO, so the serial
 * transcript is the pass/fail record independent of the console driver it
 * exercises; the visible monitor session that follows on the same image is
 * the human gate, and the simulator replay cross-checks the visible side.
 *
 * Coverage: endpoint lookup for all five classes at
 * ZOS_PROTOCOL_VERSION; the console composition rows this store implies
 * (video output, HID input); a written marker plus a typed line read
 * back through the console; a PING through the IOC endpoint with its
 * staged reply; refusal of unknown operations and forged handles; and
 * the console still working afterwards.
 */

#ifndef ZEPHYROS_TARGET_CONSOLE_INTEGRATION_SELFTEST_H
#define ZEPHYROS_TARGET_CONSOLE_INTEGRATION_SELFTEST_H

/* Result codes recorded in ace_integration_result (0 = success). */
#define ZOS_CONSOLE_INTEGRATION_OK               0x00u
#define ZOS_CONSOLE_INTEGRATION_FAIL_BOOT        0x01u /* a driver endpoint is missing    */
#define ZOS_CONSOLE_INTEGRATION_FAIL_SHAPE       0x02u /* console endpoint/protocol wrong  */
#define ZOS_CONSOLE_INTEGRATION_FAIL_BIND        0x03u /* reserved: binding rows           */
#define ZOS_CONSOLE_INTEGRATION_FAIL_IOC         0x04u /* IOC command/reply path wrong     */
#define ZOS_CONSOLE_INTEGRATION_FAIL_MALFORMED   0x05u /* a rejected call returned wrong   */
#define ZOS_CONSOLE_INTEGRATION_FAIL_CONSOLE     0x06u /* console driver wrong            */
#define ZOS_CONSOLE_INTEGRATION_FAIL_INVARIANTS  0x07u /* reserved: post-fault invariants  */

/* Recorded outcome for the host replay / smoke transcript. */
extern uint8_t ace_integration_result;

void zos_run_console_integration_selftest(void);

#endif /* ZEPHYROS_TARGET_CONSOLE_INTEGRATION_SELFTEST_H */
