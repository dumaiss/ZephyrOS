/* ZephyrOS M7 diagnostic selftest contract (test artifact).
 *
 * The kernel-side acceptance test calls zos_run_m7_selftest() from the
 * late boot hook; it validates the SYS_DIAG_* boundary directly (exact
 * results, rejections, side-effect freedom, latch/table stability) and
 * prints "m7 selftest: OK" through the normal console before the
 * temporary launcher starts the interactive monitor.
 */

#ifndef ZEPHYROS_TARGET_M7_SELFTEST_H
#define ZEPHYROS_TARGET_M7_SELFTEST_H

/* Result codes recorded in m7_selftest_result (0 = success). */
#define ZOS_M7ST_OK           0x00u
#define ZOS_M7ST_FAIL_BOOT    0x01u /* devices/console/table not ready   */
#define ZOS_M7ST_FAIL_BANK    0x02u /* BANK_INFO contract mismatch       */
#define ZOS_M7ST_FAIL_SYSINFO 0x03u /* SYSTEM_INFO record mismatch       */
#define ZOS_M7ST_FAIL_MEM     0x04u /* accepted memory op misbehaved     */
#define ZOS_M7ST_FAIL_REJECT  0x05u /* an invalid request was accepted   */
#define ZOS_M7ST_FAIL_LATCH   0x06u /* latch not restored exactly        */
#define ZOS_M7ST_FAIL_TABLE   0x07u /* process bookkeeping changed       */
#define ZOS_M7ST_FAIL_CONSOLE 0x08u /* result line could not be written  */

/* One-based check index for the failure detail word. */
#define ZOS_M7ST_CHECK_BANK_INFO    1u
#define ZOS_M7ST_CHECK_SYSTEM_INFO  2u
#define ZOS_M7ST_CHECK_MEM_ROUND    3u
#define ZOS_M7ST_CHECK_PAGE_ZERO    4u
#define ZOS_M7ST_CHECK_BAD_BLOCK    5u
#define ZOS_M7ST_CHECK_BAD_BANKS    6u
#define ZOS_M7ST_CHECK_BAD_RANGES   7u
#define ZOS_M7ST_CHECK_RESERVED     8u
#define ZOS_M7ST_CHECK_SIDE_EFFECT  9u
#define ZOS_M7ST_CHECK_IO_GOOD      10u
#define ZOS_M7ST_CHECK_IO_LATCH     11u
#define ZOS_M7ST_CHECK_LATCH_STABLE 12u

/* Recorded outcome for the host replay / smoke transcript. */
extern uint8_t m7_selftest_result;

void zos_run_m7_selftest(void);

#endif /* ZEPHYROS_TARGET_M7_SELFTEST_H */
