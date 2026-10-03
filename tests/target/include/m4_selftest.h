/* ZephyrOS M4 acceptance constants (test only).
 *
 * Shared by tests/target/m4/ (target) and the host replay in
 * tests/test_m4_sim.py; the host test parses this header so the two
 * cannot drift.  Not part of the native ABI.
 */

#ifndef ZEPHYROS_TARGET_M4_SELFTEST_H
#define ZEPHYROS_TARGET_M4_SELFTEST_H

#include "zephyros/module.h"

/* Result codes recorded in m4_selftest_result (0 = success). */
#define ZOS_M4ST_OK            0x00u
#define ZOS_M4ST_FAIL_INIT     0x01u
#define ZOS_M4ST_FAIL_CATALOG  0x02u
#define ZOS_M4ST_FAIL_ROLLBACK 0x03u
#define ZOS_M4ST_FAIL_RELOC    0x04u
#define ZOS_M4ST_FAIL_IRQ      0x05u
#define ZOS_M4ST_FAIL_HANDLER  0x06u
#define ZOS_M4ST_FAIL_BSS      0x07u
#define ZOS_M4ST_FAIL_EXHAUST  0x08u
#define ZOS_M4ST_FAIL_STATE    0x09u
#define ZOS_M4ST_FAIL_BOOT     0x0Au
#define ZOS_M4ST_FAIL_BIND     0x0Bu
#define ZOS_M4ST_FAIL_ABI1     0x0Cu
#define ZOS_M4ST_FAIL_API      0x0Du
#define ZOS_M4ST_FAIL_BIND_POLICY 0x0Eu

/* Device names registered by tests/modules/m4_probe.c, used by the target
 * self-test to verify relocated device state pointers. */
#define ZOS_M4ST_PROBE_NAME_A "m4.probe.a"
#define ZOS_M4ST_PROBE_NAME_B "m4.probe.b"
#define ZOS_M4ST_FAIL_NAME    "m4.fail"

/* Catalog names (tests/target/m4/m4_fixture_catalog.c, acceptance builds). */
#define ZOS_M4ST_CATALOG_PROBE  "m4.probe"
#define ZOS_M4ST_CATALOG_FAIL   "m4.fail"
#define ZOS_M4ST_CATALOG_FILLER "m4.filler"
#define ZOS_M4ST_CATALOG_API    "m8.api"

/* Test fixture catalog: defined in tests/target/m4/m4_fixture_catalog.c
 * and linked only into acceptance images.  Null-terminated. */
extern const struct zmod_catalog_entry zmod_fixture_catalog[];

#if ZOS_M4_SELFTEST
/* M4 loader/dynamic-driver acceptance entry (tests/target/m4/m4_test.c). */
void zos_run_m4_selftest(void);
#endif

#if ZOS_M4_FAILURE_SELFTEST
/* Failure-build rollback verification (tests/target/m4/m4_test.c). */
void zos_run_m4_failure_selftest(void);
#endif

#endif /* ZEPHYROS_TARGET_M4_SELFTEST_H */
