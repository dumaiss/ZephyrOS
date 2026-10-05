/* ZephyrOS M9 VFS-bootstrap acceptance constants (test only).
 *
 * Shared by tests/target/m9/ (target) and the host replay in
 * tests/test_m9_vfs_sim.py.  Not part of the native ABI.
 */

#ifndef ZEPHYROS_TARGET_M9_SELFTEST_H
#define ZEPHYROS_TARGET_M9_SELFTEST_H

/* Result codes recorded in m9_vfs_selftest_result (0 = success). */
#define ZOS_M9ST_OK              0x00u
#define ZOS_M9ST_FAIL_BOOT       0x01u /* VFS/module/console baseline      */
#define ZOS_M9ST_FAIL_MOUNT      0x02u /* optional-mount failure state     */
#define ZOS_M9ST_FAIL_ROOT       0x03u /* stat("/")                        */
#define ZOS_M9ST_FAIL_SYSTEM     0x04u /* stat("/SYSTEM")                  */
#define ZOS_M9ST_FAIL_ENTRY      0x06u /* per-entry stat/read mismatch     */
#define ZOS_M9ST_FAIL_CASE       0x07u /* case-insensitive resolution      */
#define ZOS_M9ST_FAIL_NORMALIZE  0x08u /* '.'/'..'/'//' normalization      */
#define ZOS_M9ST_FAIL_ENOENT     0x09u /* missing-path error               */
#define ZOS_M9ST_FAIL_ENOTDIR    0x0Au /* lookup below a file              */
#define ZOS_M9ST_FAIL_EROFS      0x0Bu /* mutation not refused             */
#define ZOS_M9ST_FAIL_READDIR    0x0Cu /* root enumeration                 */
#define ZOS_M9ST_FAIL_EXEC       0x0Du /* VFS executable gate              */
#define ZOS_M9ST_FAIL_CONSOLE    0x0Eu /* acceptance console line failed   */
#define ZOS_M9ST_FAIL_FLAT       0x0Fu /* flat mount selection (/SYSTEMX)  */
#define ZOS_M9ST_FAIL_DEEPER     0x10u /* deeper path: ENOTDIR vs ENOENT   */
#define ZOS_M9ST_FAIL_MUTATE     0x11u /* null mutation operation not EROFS*/
#define ZOS_M9ST_FAIL_FLAGS      0x12u /* open flag validation not EINVAL  */

#if ZOS_M9_VFS_SELFTEST
void zos_run_m9_vfs_selftest(void);
#endif

#endif /* ZEPHYROS_TARGET_M9_SELFTEST_H */
