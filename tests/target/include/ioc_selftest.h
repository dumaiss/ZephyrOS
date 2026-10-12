/* ZephyrOS IOC acceptance selftest contract (test artifact, package 41).
 *
 * The nucleus-side acceptance runner (tests/target/support/target_test.c,
 * called from kernel/main.c's late boot hook) calls
 * zos_run_ioc_selftest() once the driver loader has brought the image's
 * driver set (sio, ioc, console) up in bank 6.  The diagnostic itself
 * lives in tests/target/ioc/ioc_test.c and reports through the early SIO;
 * its header comment is the authoritative description of what it proves.
 */

#ifndef ZEPHYROS_TARGET_IOC_SELFTEST_H
#define ZEPHYROS_TARGET_IOC_SELFTEST_H

/* Result codes recorded in ace_ioc_selftest_result (0 = success). */
#define ZOS_IOCSELFTEST_OK            0x00u
#define ZOS_IOCSELFTEST_FAIL_ENDPOINT 0x01u /* no IOC endpoint at version 1 */
#define ZOS_IOCSELFTEST_FAIL_READY    0x02u /* ZIOC_OP_READY is not 1        */
#define ZOS_IOCSELFTEST_FAIL_COMMAND  0x03u /* PING exchange/reply failure   */
#define ZOS_IOCSELFTEST_FAIL_STATUS   0x04u /* status word outside the mask  */
#define ZOS_IOCSELFTEST_FAIL_BULK     0x05u /* staged bulk phase failure     */
#define ZOS_IOCSELFTEST_FAIL_REFUSED  0x06u /* a malformed call was served   */
#define ZOS_IOCSELFTEST_PENDING       0x0Eu /* firmware has no writable
					      * /SHARED driver: not a pass
					      * and not a failure - never
					      * acceptance evidence */

/* Recorded outcome for the host replay. */
extern uint8_t ace_ioc_selftest_result;

void zos_run_ioc_selftest(void);

#endif /* ZEPHYROS_TARGET_IOC_SELFTEST_H */
