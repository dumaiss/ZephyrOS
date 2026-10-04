/* ZephyrOS loader acceptance constants (test only).
 *
 * Shared by tests/target/loader/loader_test.c and the host replay in
 * tests/test_subsys_sim.py; the host test parses this header so the
 * target codes and the transcript expectations cannot drift.  Not part
 * of the native ABI.
 */

#ifndef ZEPHYROS_TARGET_LOADER_SELFTEST_H
#define ZEPHYROS_TARGET_LOADER_SELFTEST_H

/* Result codes recorded in loader_selftest_result (0 = success). */
#define ZOS_LDST_OK             0x00u
#define ZOS_LDST_FAIL_BOOT      0x01u /* boot module/console baseline absent  */
#define ZOS_LDST_FAIL_LOOKUP    0x02u /* fixture entry missing from ROMFS     */
#define ZOS_LDST_FAIL_VALIDATE  0x03u /* malformed container accepted         */
#define ZOS_LDST_FAIL_ROLLBACK  0x04u /* failed init left device/claim/allocation */
#define ZOS_LDST_FAIL_LEAK      0x05u /* arena/descriptor count changed       */
#define ZOS_LDST_FAIL_RELOC     0x06u /* probe pointers not relocated in place */
#define ZOS_LDST_FAIL_BSS       0x07u /* BSS not zeroed before init           */
#define ZOS_LDST_FAIL_HANDLER   0x08u /* handler-address validation wrong     */
#define ZOS_LDST_FAIL_IRQ       0x09u /* init-time claim not owned by module  */
#define ZOS_LDST_FAIL_EXHAUST   0x0Au /* arena exhaustion not diagnosed       */
#define ZOS_LDST_FAIL_CONSOLE   0x0Bu /* acceptance console line failed       */

/* Loader fixture identities and their recovery-store entry names
 * (tests/subsys/manifest.json).  The registry name is what the loader
 * records; the file name is what the test opens through VFS. */
#define ZOS_LDST_PROBE_NAME    "m4.probe"
#define ZOS_LDST_PROBE_FILE    "m4probe.zmod"
#define ZOS_LDST_FILLER_NAME   "m4.filler"
#define ZOS_LDST_FILLER_FILE   "m4filler.zmod"
#define ZOS_LDST_FAIL_NAME     "m4.fail"
#define ZOS_LDST_FAIL_FILE     "m4fail.zmod"
#define ZOS_LDST_BADMAGIC_NAME "bad.magic"
#define ZOS_LDST_BADMAGIC_FILE "badmagic.zmod"
#define ZOS_LDST_TRUNC_NAME    "short.mod"
#define ZOS_LDST_TRUNC_FILE    "truncated.zmod"
#define ZOS_LDST_PROBE_A       "m4.probe.a"
#define ZOS_LDST_PROBE_B       "m4.probe.b"

/* The boot module every subsystem image loads from the recovery store. */
#define ZOS_LDST_BOOT_NAME     "sio"

#if ZOS_SUBSYS_SELFTEST
/* Loader acceptance entry (tests/target/loader/loader_test.c). */
void zos_run_loader_selftest(void);
#endif

#endif /* ZEPHYROS_TARGET_LOADER_SELFTEST_H */
