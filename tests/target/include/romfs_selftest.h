/* ZephyrOS ROMFS-reader acceptance constants (test only).
 *
 * Shared by tests/target/romfs/ (target) and the host replay in
 * tests/test_romfs_sim.py; the host test parses this header so the two
 * cannot drift.  Not part of the native ABI.
 */

#ifndef ZEPHYROS_TARGET_ROMFS_SELFTEST_H
#define ZEPHYROS_TARGET_ROMFS_SELFTEST_H

/* Result codes recorded in romfs_selftest_result (0 = success). */
#define ZOS_RFST_OK            0x00u
#define ZOS_RFST_FAIL_BOOT     0x01u
#define ZOS_RFST_FAIL_LOOKUP   0x02u
#define ZOS_RFST_FAIL_SIZE     0x03u
#define ZOS_RFST_FAIL_SUM      0x04u
#define ZOS_RFST_FAIL_EOF      0x05u
#define ZOS_RFST_FAIL_FIRST    0x06u
#define ZOS_RFST_FAIL_LAST     0x07u
#define ZOS_RFST_FAIL_CROSS    0x08u
#define ZOS_RFST_FAIL_CONSOLE  0x09u

/* Acceptance-manifest geometry (tests/subsys/manifest.json, packed by
 * tools/romfspack.py into the same logical stream as production).  The
 * first entry's payload offset -- the file-relative position of the
 * 48-KiB logical window boundary -- is emitted as CROSSPAGE_OFFSET into
 * the generated romfs_expect.inc, so adding a manifest entry cannot make
 * the target read the boundary at the wrong place. */
#define ZOS_RFST_WINDOW          0xC000u
#define ZOS_RFST_CROSSPAGE_NAME  "crosspage.bin"
#define ZOS_RFST_CROSSPAGE_SIZE  0xC040u

#if ZOS_SUBSYS_SELFTEST
/* ROMFS-reader acceptance entry (tests/target/romfs/romfs_test.c). */
void zos_run_romfs_selftest(void);
#endif

#endif /* ZEPHYROS_TARGET_ROMFS_SELFTEST_H */
