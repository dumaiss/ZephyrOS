/* ZephyrOS M8 integration acceptance selftest contract (test artifact).
 *
 * The kernel-side integration test calls zos_run_m8_selftest() from the
 * late boot hook after the production-style boot: SIO + IOC + HID + the
 * V9958 video/console device are all loaded from the recovery store and
 * the single console_bind_defaults() has selected the preferred
 * HID-input / V9958-output pair.  The unchanged M7 monitor launcher
 * runs after this test, so the same image carries the interactive
 * session through the binding.
 *
 * Control and outcome lines go through the direct early SIO, so the
 * serial transcript is the pass/fail record independent of every bound
 * console; the visible monitor session is the human gate.
 *
 * Coverage:
 *
 *   - all four resident modules are discoverable by class/capability
 *     and the default binding is exactly the HID input plus V9958
 *     output handles;
 *   - the IOC class exposes its four operations; repeated PING
 *     exchanges (this image's command/status slice; the complete bulk
 *     READY/DONE lifecycle on hardware lives in the dedicated IOC
 *     acceptance image) return validated replies;
 *   - malformed/null/class/capability calls fail with exact codes and
 *     are proven harmless: the device table, the module allocation
 *     count and both console directions are unchanged afterwards, and
 *     the visible console output marker still arrives;
 *   - the early/panic SIO path stays independent of the binding (the
 *     transcript itself is the proof: it is produced while the normal
 *     console output is the V9958).
 */

#ifndef ZEPHYROS_TARGET_M8_SELFTEST_H
#define ZEPHYROS_TARGET_M8_SELFTEST_H

/* Result codes recorded in m8_selftest_result (0 = success). */
#define ZOS_M8_OK               0x00u
#define ZOS_M8_FAIL_BOOT        0x01u /* a resident module is missing     */
#define ZOS_M8_FAIL_SHAPE       0x02u /* class/capability shape wrong     */
#define ZOS_M8_FAIL_BIND        0x03u /* default binding wrong            */
#define ZOS_M8_FAIL_IOC         0x04u /* IOC command/status path wrong    */
#define ZOS_M8_FAIL_MALFORMED   0x05u /* a rejected call returned wrong   */
#define ZOS_M8_FAIL_CONSOLE     0x06u /* console service wrong            */
#define ZOS_M8_FAIL_INVARIANTS  0x07u /* registry/granules/table changed  */

/* Recorded outcome for the host replay / smoke transcript. */
extern uint8_t m8_selftest_result;

void zos_run_m8_selftest(void);

#endif /* ZEPHYROS_TARGET_M8_SELFTEST_H */
