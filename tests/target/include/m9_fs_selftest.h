/* ZephyrOS M9 filesystem syscall acceptance constants (test only).
 *
 * Shared by tests/target/m9/m9_fs_test.c (target), the host replay in
 * tests/test_syscall_sim.py, and the host ABI pinning in tests/test_m9_fs.py.
 * Not part of the native ABI.
 */

#ifndef ZEPHYROS_TARGET_M9_FS_SELFTEST_H
#define ZEPHYROS_TARGET_M9_FS_SELFTEST_H

/* Result codes recorded in m9_fs_selftest_result (0 = success). */
#define ZOS_M9FST_OK            0x00u
#define ZOS_M9FST_FAIL_EXEC     0x01u /* fstest.zex launch failed        */
#define ZOS_M9FST_FAIL_STATUS   0x02u /* fstest.zex reported failure     */
#define ZOS_M9FST_FAIL_LEAK     0x03u /* descriptor/CWD pool not released*/
#define ZOS_M9FST_FAIL_LAUNCHER 0x04u /* second exec launcher check failed*/

#if ZOS_M9_FS_SELFTEST
void zos_run_m9_fs_selftest(void);
#endif

#endif /* ZEPHYROS_TARGET_M9_FS_SELFTEST_H */
