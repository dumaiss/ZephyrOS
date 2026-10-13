/* KonaOS IOC acceptance selftest contract (test artifact).
 *
 * The nucleus-side acceptance runner (tests/target/support/target_test.c,
 * called from kernel/main.c's late boot hook) calls
 * kos_run_ioc_selftest() once the driver loader has brought the image's
 * driver set (sio, ioc, console) up in bank 6.  The diagnostic itself
 * lives in tests/target/ioc/ioc_test.c and reports through the early SIO;
 * its header comment is the authoritative description of what it proves.
 */

#ifndef KONAOS_TARGET_IOC_SELFTEST_H
#define KONAOS_TARGET_IOC_SELFTEST_H

/* Result codes recorded in ace_ioc_selftest_result (0 = success). */
#define KOS_IOCSELFTEST_OK            0x00u
#define KOS_IOCSELFTEST_FAIL_ENDPOINT 0x01u /* no IOC endpoint at version 1 */
#define KOS_IOCSELFTEST_FAIL_READY    0x02u /* KIOC_OP_READY is not 1        */
#define KOS_IOCSELFTEST_FAIL_COMMAND  0x03u /* PING exchange/reply failure   */
#define KOS_IOCSELFTEST_FAIL_STATUS   0x04u /* status word outside the mask  */
#define KOS_IOCSELFTEST_FAIL_BULK     0x05u /* staged bulk phase failure     */
#define KOS_IOCSELFTEST_FAIL_REFUSED  0x06u /* a malformed call was served   */
#define KOS_IOCSELFTEST_PENDING       0x0Eu /* firmware has no writable
					      * /SHARED driver: not a pass
					      * and not a failure - never
					      * acceptance evidence */

/* Recorded outcome for the host replay. */
extern uint8_t ace_ioc_selftest_result;

void kos_run_ioc_selftest(void);

#endif /* KONAOS_TARGET_IOC_SELFTEST_H */
