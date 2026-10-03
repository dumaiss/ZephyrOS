/* ZephyrOS M6 process/ZEX acceptance constants (test only).
 *
 * Shared by tests/target/m6/ (target), tests/m6/ (fixtures), and the
 * host replay in tests/test_m6_sim.py; the host tests parse this header
 * so the two cannot drift.  Not part of the native ABI.
 */

#ifndef ZEPHYROS_TARGET_M6_SELFTEST_H
#define ZEPHYROS_TARGET_M6_SELFTEST_H

/* Result codes recorded in m6_selftest_result (0 = success). */
#define ZOS_M6ST_OK            0x00u
#define ZOS_M6ST_FAIL_BOOT     0x01u /* module/console baseline missing   */
#define ZOS_M6ST_FAIL_EXEC     0x02u /* process_exec refused the child    */
#define ZOS_M6ST_FAIL_STATUS   0x03u /* child exit status mismatch        */
#define ZOS_M6ST_FAIL_LEAK     0x04u /* slot/bank leaked after a run      */
#define ZOS_M6ST_FAIL_REPEAT   0x05u /* repeated exec/exit cycle mismatch */
#define ZOS_M6ST_FAIL_BANK     0x06u /* two-bank run used the wrong bank  */
#define ZOS_M6ST_FAIL_FAULT    0x07u /* raw-return child did not EFAULT   */
#define ZOS_M6ST_FAIL_REJECT   0x08u /* malformed ZEX not rejected by code*/
#define ZOS_M6ST_FAIL_RESERVE  0x09u /* allocator reservation mismatch    */
#define ZOS_M6ST_FAIL_ARG      0x0Au /* API argument handling mismatch    */
#define ZOS_M6ST_FAIL_CONSOLE  0x0Bu /* acceptance console line failed    */

/* zextest program exit status: pass = PASS | bank, fail = FAIL | code.
 * A bank in the low byte proves which application bank executed the
 * same ROMFS payload. */
#define ZOS_M6ST_ZEXTEST_PASS 0xA500u
#define ZOS_M6ST_ZEXTEST_FAIL 0x0100u

/* zextest internal check codes (low byte of a failing status). */
#define ZOS_M6ST_ZT_FAIL_BSS     0x01u
#define ZOS_M6ST_ZT_FAIL_IMAGE   0x02u
#define ZOS_M6ST_ZT_FAIL_STACK   0x03u
#define ZOS_M6ST_ZT_FAIL_GETINFO 0x04u
#define ZOS_M6ST_ZT_FAIL_WRITE   0x05u

/* Acceptance-manifest names (tests/m6/manifest.json). */
#define ZOS_M6ST_ZEXTEST_NAME "zextest.zex"
#define ZOS_M6ST_RETURN_NAME  "zex-return.zex"
#define ZOS_M6ST_FAILURE_NAME "zextest.zex" /* truncated in the failure image */
#define ZOS_M6ST_FAILURE_OK_NAME "zexok.zex"

/* Fixture descriptor emitted into the generated m6_expect.inc table. */
struct m6_fixture_desc {
	const char *name; /* ROMFS name */
	int code;         /* exact process_exec result */
};

#if ZOS_M6_SELFTEST
void zos_run_m6_selftest(void);
#endif
#if ZOS_M6_FAILURE_SELFTEST
void zos_run_m6_failure_selftest(void);
#endif

#endif /* ZEPHYROS_TARGET_M6_SELFTEST_H */
