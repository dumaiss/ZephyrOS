/* ZephyrOS M8 IOC command-lane selftest contract (test artifact).
 *
 * The kernel-side acceptance test calls zos_run_m8_ioc_selftest() from
 * the late boot hook after the production-style boot (SIO and IOC loaded
 * from ROMFS, console bound).  It reports through the direct early SIO
 * and exercises the registered `ioc.0` device:
 *
 *   - discovers ZDEV_IOC by class plus command/status capabilities and
 *     confirms the bulk capabilities and pointers are absent in this
 *     stage;
 *   - performs repeated PING exchanges (deterministic echo pattern) to
 *     prove the persistent link survives ordinary commands;
 *   - prints and validates the controller firmware level returned by
 *     PING (74 / 4Ah for the pinned deployment);
 *   - identifies the implemented host contract as transport level 08h;
 *   - samples the semantic status interface and rejects any bit outside
 *     ZIOC_STATUS_HID_PENDING.
 */

#ifndef ZEPHYROS_TARGET_M8_IOC_SELFTEST_H
#define ZEPHYROS_TARGET_M8_IOC_SELFTEST_H

/* Result codes recorded in m8_ioc_selftest_result (0 = success). */
#define ZOS_M8IOC_OK          0x00u
#define ZOS_M8IOC_FAIL_BOOT   0x01u /* no ioc.0 / bad interface shape    */
#define ZOS_M8IOC_FAIL_PING   0x02u /* PING transport/protocol failure   */
#define ZOS_M8IOC_FAIL_ECHO   0x03u /* PING echo pattern mismatch        */
#define ZOS_M8IOC_FAIL_LEVEL  0x04u /* controller level not 4Ah          */
#define ZOS_M8IOC_FAIL_STATUS 0x05u /* status() returned foreign bits    */
#define ZOS_M8IOC_FAIL_REPEAT 0x06u /* a repeated PING did not repeat    */

/* Recorded outcome for the host replay / smoke transcript. */
extern uint8_t m8_ioc_selftest_result;

void zos_run_m8_ioc_selftest(void);

#endif /* ZEPHYROS_TARGET_M8_IOC_SELFTEST_H */
