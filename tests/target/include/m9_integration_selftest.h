/* ZephyrOS M9 integration acceptance constants (test only, package 31).
 *
 * Shared by tests/target/m9/m9_selftest.c (target), the simulator replay
 * in tests/test_m9_selftest_sim.py, and the host checks in
 * tests/test_m9_selftest.py.  Not part of the native ABI.
 */

#ifndef ZEPHYROS_TARGET_M9_INTEGRATION_SELFTEST_H
#define ZEPHYROS_TARGET_M9_INTEGRATION_SELFTEST_H

/* Result codes recorded in m9_integration_selftest_result (0 = success). */
#define ZOS_M9IST_OK           0x00u
#define ZOS_M9IST_FAIL_FSTEST  0x01u /* fstest.zex absent or failed        */
#define ZOS_M9IST_FAIL_EXEC    0x02u /* the depth probe could not be run   */
#define ZOS_M9IST_FAIL_DEPTH   0x03u /* the chain did not reach the limit  */
#define ZOS_M9IST_FAIL_FAULT   0x04u /* stale/write-fault probe inconsistent */
#define ZOS_M9IST_FAIL_LEAK    0x05u /* a pool was not back to baseline    */

#if ZOS_M9_SELFTEST
void zos_run_m9_selftest(void);
#endif

#endif /* ZEPHYROS_TARGET_M9_INTEGRATION_SELFTEST_H */
