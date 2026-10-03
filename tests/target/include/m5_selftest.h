/* ZephyrOS M5 acceptance constants (test only).
 *
 * Shared by tests/target/m5/ (target) and the host replay in
 * tests/test_romfs_sim.py; the host test parses this header so the two
 * cannot drift.  Not part of the native ABI.
 */

#ifndef ZEPHYROS_TARGET_M5_SELFTEST_H
#define ZEPHYROS_TARGET_M5_SELFTEST_H

/* Result codes recorded in m5_selftest_result (0 = success). */
#define ZOS_M5ST_OK            0x00u
#define ZOS_M5ST_FAIL_BOOT     0x01u
#define ZOS_M5ST_FAIL_LOOKUP   0x02u
#define ZOS_M5ST_FAIL_SIZE     0x03u
#define ZOS_M5ST_FAIL_SUM      0x04u
#define ZOS_M5ST_FAIL_EOF      0x05u
#define ZOS_M5ST_FAIL_FIRST    0x06u
#define ZOS_M5ST_FAIL_LAST     0x07u
#define ZOS_M5ST_FAIL_CROSS    0x08u
#define ZOS_M5ST_FAIL_CONSOLE  0x09u

/* Acceptance-manifest geometry (tests/m5/manifest.json, packed by
 * tools/romfspack.py into the same logical stream as production).
 * CROSSPAGE_OFFSET is the first entry's payload offset: 16-byte header +
 * 2 x 36-byte directory entries. */
#define ZOS_M5ST_WINDOW          0xC000u
#define ZOS_M5ST_CROSSPAGE_NAME  "m5-crosspage.bin"
#define ZOS_M5ST_CROSSPAGE_SIZE  0xC040u
#define ZOS_M5ST_CROSSPAGE_OFFSET 0x0058u

#if ZOS_M5_SELFTEST
/* M5 ROMFS acceptance entry (tests/target/m5/m5_test.c). */
void zos_run_m5_selftest(void);
#endif

#endif /* ZEPHYROS_TARGET_M5_SELFTEST_H */
