/* ZephyrOS M9 SYS_EXEC acceptance constants (test only, package 28).
 *
 * Shared by tests/target/m9/m9_exec_test.c (target) and the host replay in
 * tests/test_m9_exec_sim.py.  Not part of the native ABI.
 */

#ifndef ZEPHYROS_TARGET_M9_EXEC_SELFTEST_H
#define ZEPHYROS_TARGET_M9_EXEC_SELFTEST_H

/* Result codes recorded in m9_exec_selftest_result (0 = success). */
#define ZOS_M9EST_OK          0x00u
#define ZOS_M9EST_FAIL_EXEC   0x01u /* process_exec refused a program      */
#define ZOS_M9EST_FAIL_STATUS 0x02u /* child exit status mismatch          */
#define ZOS_M9EST_FAIL_DEPTH  0x03u /* depth probe did not stop at 4       */
#define ZOS_M9EST_FAIL_LEAK   0x04u /* slot/bank/descriptor leaked         */

#if ZOS_M9_EXEC_SELFTEST
void zos_run_m9_exec_selftest(void);
#endif

#endif /* ZEPHYROS_TARGET_M9_EXEC_SELFTEST_H */
