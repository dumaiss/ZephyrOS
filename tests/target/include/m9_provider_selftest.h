/* ZephyrOS M9 writable-provider acceptance constants (test only).
 *
 * Shared by tests/target/m9/m9_provider_test.c (target) and the host replay
 * in tests/test_m9_provider_sim.py.  The `m9p_stats` mirror is the exact
 * little-endian image the ramfs module exposes through its /STATS control.
 */

#ifndef ZEPHYROS_TARGET_M9_PROVIDER_SELFTEST_H
#define ZEPHYROS_TARGET_M9_PROVIDER_SELFTEST_H

/* Result codes recorded in m9_provider_selftest_result (0 = success). */
#define ZOS_M9PST_OK              0x00u
#define ZOS_M9PST_FAIL_BOOT       0x01u /* phase/module baseline           */
#define ZOS_M9PST_FAIL_REGISTRY   0x02u /* provider registry/rollback      */
#define ZOS_M9PST_FAIL_MOUNT      0x03u /* / and /R2 mount state           */
#define ZOS_M9PST_FAIL_MKDIR      0x04u /* mkdir/rmdir semantics           */
#define ZOS_M9PST_FAIL_OPEN       0x05u /* open flags and EISDIR/ENOTDIR   */
#define ZOS_M9PST_FAIL_WRITE      0x06u /* write and stat size             */
#define ZOS_M9PST_FAIL_READ       0x07u /* read-back byte compare          */
#define ZOS_M9PST_FAIL_SEEK       0x08u /* 32-bit seek origins/negatives   */
#define ZOS_M9PST_FAIL_RENAME     0x09u /* rename within a mount           */
#define ZOS_M9PST_FAIL_UNLINK     0x0Au /* unlink/ENOENT                   */
#define ZOS_M9PST_FAIL_ROOTS      0x0Bu /* root merge and shadowing        */
#define ZOS_M9PST_FAIL_SIZES      0x0Cu /* section 33 size set             */
#define ZOS_M9PST_FAIL_STALE      0x0Du /* stale-handle control            */
#define ZOS_M9PST_FAIL_WRITEFAIL  0x0Eu /* failed write leaves offset/size */
#define ZOS_M9PST_FAIL_EXDEV      0x0Fu /* cross-mount rename              */
#define ZOS_M9PST_FAIL_ERRORS     0x10u /* mountpoint/ENODEV/EROFS errors  */
#define ZOS_M9PST_FAIL_COUNTERS   0x11u /* call counters and open count    */
#define ZOS_M9PST_FAIL_UNKNOWN    0x7Fu /* unrecognized mapping            */

/* Mirror of the ramfs /STATS image: open-handle count then one call counter
 * per non-mount operation (mount, open, close, read, write, sync, stat,
 * opendir, readdir, closedir, mkdir, rmdir, unlink, rename). */
#define ZOS_M9PST_CALLS 14u

struct m9p_stats {
	uint16_t open_handles;
	uint16_t calls[ZOS_M9PST_CALLS];
};

#if defined(__SDCC)
_Static_assert(sizeof(struct m9p_stats) == 30,
	       "m9p_stats must be 30 bytes");
#endif

#if ZOS_M9_PROVIDER_SELFTEST
void zos_run_m9_provider_selftest(void);
#endif

#endif /* ZEPHYROS_TARGET_M9_PROVIDER_SELFTEST_H */
