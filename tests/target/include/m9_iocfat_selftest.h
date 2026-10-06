/* ZephyrOS M9 IOC-FAT provider acceptance constants (test only).
 *
 * Shared by tests/target/m9/m9_iocfat_test.c (target), the simulator replay
 * in tests/test_iocfat_sim.py, and the host checks in tests/test_iocfat.py.
 * Not part of the native ABI.
 */

#ifndef ZEPHYROS_TARGET_M9_IOCFAT_SELFTEST_H
#define ZEPHYROS_TARGET_M9_IOCFAT_SELFTEST_H

/* Result codes recorded in m9_iocfat_selftest_result (0 = success). */
#define ZOS_M9IOFAT_OK              0x00u
#define ZOS_M9IOFAT_FAIL_MOUNT      0x01u /* `/` not served by iocfat       */
#define ZOS_M9IOFAT_FAIL_OPEN       0x02u
#define ZOS_M9IOFAT_FAIL_WRITE      0x03u
#define ZOS_M9IOFAT_FAIL_READ       0x04u
#define ZOS_M9IOFAT_FAIL_COMPARE    0x05u
#define ZOS_M9IOFAT_FAIL_STAT       0x06u
#define ZOS_M9IOFAT_FAIL_TRUNC      0x07u
#define ZOS_M9IOFAT_FAIL_MKDIR      0x08u
#define ZOS_M9IOFAT_FAIL_RMDIR      0x09u
#define ZOS_M9IOFAT_FAIL_UNLINK     0x0Au
#define ZOS_M9IOFAT_FAIL_RENAME     0x0Bu
#define ZOS_M9IOFAT_FAIL_EXDEV      0x0Cu
#define ZOS_M9IOFAT_FAIL_ENOTEMPTY  0x0Du
#define ZOS_M9IOFAT_FAIL_READDIR    0x0Eu
#define ZOS_M9IOFAT_FAIL_EMFILE     0x0Fu
#define ZOS_M9IOFAT_FAIL_EROFS      0x10u
#define ZOS_M9IOFAT_FAIL_STALE      0x11u
#define ZOS_M9IOFAT_FAIL_EISDIR     0x12u
#define ZOS_M9IOFAT_FAIL_UNEXPECTED 0x13u
#define ZOS_M9IOFAT_FAIL_FSTEST     0x14u /* native fstest.zex failed      */
#define ZOS_M9IOFAT_FAIL_NAME       0x15u /* 8.3 name rule                 */

#if ZOS_M9_IOCFAT_SELFTEST
void zos_run_m9_iocfat_selftest(void);
#endif

#endif /* ZEPHYROS_TARGET_M9_IOCFAT_SELFTEST_H */
