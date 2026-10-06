/* ZephyrOS M8 IOC acceptance selftest contract (test artifact).
 *
 * The kernel-side acceptance test calls zos_run_m8_ioc_selftest() from
 * the late boot hook after the production-style boot (SIO and IOC loaded
 * from ROMFS, console bound).  It reports through the direct early SIO
 * and exercises the registered `ioc.0` device:
 *
 *   - discovers ZDEV_IOC by class plus all four capabilities and
 *     requires every operation pointer, bulk included, to be present;
 *   - performs repeated PING exchanges (deterministic echo pattern) to
 *     prove the persistent link survives ordinary commands;
 *   - prints and validates the controller firmware level returned by
 *     PING (74 / 4Ah for the pinned deployment);
 *   - identifies the implemented host contract as transport level 08h;
 *   - drives the complete READY/DONE bulk lifecycle over the production
 *     /SHARED file service: write 1/128/512-byte sections with matching
 *     DONE records, read them back and verify, then delete the dedicated
 *     acceptance file and prove the delete by a failing reopen;
 *   - a deployed firmware that answers the writable open with the
 *     unknown-command class reports the bulk-write criterion PENDING
 *     (explicit PENDING marker, never the OK marker, never acceptance
 *     evidence); a real filesystem/media failure status is a failure;
 *   - samples the semantic status interface and rejects any bit outside
 *     ZIOC_STATUS_HID_PENDING.
 */

#ifndef ZEPHYROS_TARGET_M8_IOC_SELFTEST_H
#define ZEPHYROS_TARGET_M8_IOC_SELFTEST_H

/* Result codes recorded in m8_ioc_selftest_result (0 = success). */
#define ZOS_M8IOC_OK              0x00u
#define ZOS_M8IOC_FAIL_BOOT       0x01u /* no ioc.0 / bad interface shape */
#define ZOS_M8IOC_FAIL_PING       0x02u /* PING transport/protocol failure */
#define ZOS_M8IOC_FAIL_ECHO       0x03u /* PING echo pattern mismatch      */
#define ZOS_M8IOC_FAIL_LEVEL      0x04u /* controller level not 4Ah        */
#define ZOS_M8IOC_FAIL_STATUS     0x05u /* status() returned foreign bits  */
#define ZOS_M8IOC_FAIL_REPEAT     0x06u /* a repeated PING did not repeat  */
#define ZOS_M8IOC_FAIL_BULK_OPEN  0x07u /* /SHARED open/close failure      */
#define ZOS_M8IOC_FAIL_BULK_WRITE 0x08u /* write command/phase failure     */
#define ZOS_M8IOC_FAIL_BULK_DONE  0x09u /* DONE missing or not matching    */
#define ZOS_M8IOC_FAIL_BULK_READ  0x0Au /* read command/phase failure      */
#define ZOS_M8IOC_FAIL_BULK_DATA  0x0Bu /* read-back pattern mismatch      */
#define ZOS_M8IOC_FAIL_BULK_CLOSE 0x0Cu /* close failure                   */
#define ZOS_M8IOC_FAIL_BULK_DELETE 0x0Du /* delete failure                 */
#define ZOS_M8IOC_PENDING         0x0Eu /* firmware has no writable /SHARED
					  * service; not a pass and not a
					  * failure - never acceptance evidence */

/* Recorded outcome for the host replay / smoke transcript. */
extern uint8_t m8_ioc_selftest_result;

void zos_run_m8_ioc_selftest(void);

#endif /* ZEPHYROS_TARGET_M8_IOC_SELFTEST_H */
