/* KonaOS process/EXE acceptance constants (test only).
 *
 * Shared by tests/target/process/ (target) and the tests/subsys/ fixture
 * generators, which parse this header so the two cannot drift.  Not part of the native ABI.
 */

#ifndef KONAOS_TARGET_PROCESS_SELFTEST_H
#define KONAOS_TARGET_PROCESS_SELFTEST_H

/* Result codes recorded in m6_selftest_result (0 = success). */
#define KOS_PCST_OK            0x00u
#define KOS_PCST_FAIL_BOOT     0x01u /* module/console baseline missing   */
#define KOS_PCST_FAIL_EXEC     0x02u /* process_exec refused the child    */
#define KOS_PCST_FAIL_STATUS   0x03u /* child exit status mismatch        */
#define KOS_PCST_FAIL_LEAK     0x04u /* slot/bank leaked after a run      */
#define KOS_PCST_FAIL_REPEAT   0x05u /* repeated exec/exit cycle mismatch */
#define KOS_PCST_FAIL_BANK     0x06u /* two-bank run used the wrong bank  */
#define KOS_PCST_FAIL_FAULT    0x07u /* raw-return child did not EFAULT   */
#define KOS_PCST_FAIL_REJECT   0x08u /* malformed EXE not rejected by code*/
#define KOS_PCST_FAIL_RESERVE  0x09u /* allocator reservation mismatch    */
#define KOS_PCST_FAIL_ARG      0x0Au /* API argument handling mismatch    */
#define KOS_PCST_FAIL_CONSOLE  0x0Bu /* acceptance console line failed    */
#define KOS_PCST_FAIL_RESOURCE 0x0Cu /* ROMFS resource import mismatch    */

/* exetest program exit status: pass = PASS | bank, fail = FAIL | code.
 * A bank in the low byte proves which application bank executed the
 * same ROMFS payload. */
#define KOS_PCST_EXETEST_PASS 0xA500u
#define KOS_PCST_EXETEST_FAIL 0x0100u

/* exetest internal check codes (low byte of a failing status). */
#define KOS_PCST_ZT_FAIL_BSS     0x01u
#define KOS_PCST_ZT_FAIL_IMAGE   0x02u
#define KOS_PCST_ZT_FAIL_STACK   0x03u
#define KOS_PCST_ZT_FAIL_GETINFO 0x04u
#define KOS_PCST_ZT_FAIL_WRITE   0x05u

/* Loader-outcome selftest codes (0x0D-0x10 are the EXE v2
 * group: BSS/bank zeroing, DATA segments, the >64-KiB container and the
 * segment-allocation rollback). */
#define KOS_PCST_FAIL_EXE2_BSS  0x0Du /* BSS zeroed / bank not cleared   */
#define KOS_PCST_FAIL_EXE2_SEG  0x0Eu /* DATA segment wiring             */
#define KOS_PCST_FAIL_EXE2_BIG  0x0Fu /* container over 64 KiB           */
#define KOS_PCST_FAIL_EXE2_ROLL 0x10u /* segment allocation rollback     */

/* --- EXE v2 loader-outcome programs ----------------
 *
 * Three SDK programs in the subsystem store that exercise the loader
 * outcomes the v2 contract names: the primary BSS is zeroed after the
 * whole-bank clear was removed, the bank outside image/BSS/stack is NOT
 * cleared, a DATA segment's contents and BSS are placed correctly, and a
 * container over 64 KiB loads.  Each exits with PASS | bank. */
#define KOS_PCST_PROBE_NAME "exe2probe.exe"
#define KOS_PCST_SEG_NAME   "exe2seg.exe"
#define KOS_PCST_BIG_NAME   "exe2big.exe"

#define KOS_PCST_EXE2_PASS 0xB500u
#define KOS_PCST_EXE2_FAIL 0x0200u

/* Program check codes (low byte of a failing status). */
#define KOS_PCST_Z2_FAIL_BSS     0x01u /* BSS not zeroed                  */
#define KOS_PCST_Z2_FAIL_MARK    0x02u /* bank was cleared                */
#define KOS_PCST_Z2_FAIL_IMAGE   0x03u /* image bytes wrong               */
#define KOS_PCST_Z2_FAIL_SEGINFO 0x04u /* SYS_SEG_INFO wrong/not refused  */
#define KOS_PCST_Z2_FAIL_SEGDATA 0x05u /* segment payload wrong           */
#define KOS_PCST_Z2_FAIL_SEGBSS  0x06u /* segment BSS not zeroed          */
#define KOS_PCST_Z2_FAIL_FREE    0x07u /* SYS_XMEM_FREE accepted a segment*/
#define KOS_PCST_Z2_FAIL_WRITE   0x08u /* segment write-back failed       */
#define KOS_PCST_Z2_FAIL_GETINFO 0x09u /* GETINFO failed                  */
#define KOS_PCST_Z2_FAIL_LATCH   0x0Au /* not mode 10 with P = 0          */

/* The pattern the kernel test writes into the child's bank outside its
 * image, BSS and stack.  The loader must leave it alone (4.8). */
#define KOS_PCST_EXE2_MARK_ADDR 0xD000u
#define KOS_PCST_EXE2_MARK_SIZE 4u
#define KOS_PCST_EXE2_MARK_0 0x5Au
#define KOS_PCST_EXE2_MARK_1 0x69u
#define KOS_PCST_EXE2_MARK_2 0xA5u
#define KOS_PCST_EXE2_MARK_3 0xC3u

/* Segment fixture pins: the sizes and patterns
 * tests/subsys/gen_exe2_segments.py writes and the programs expect.  The
 * kernel test reads the exact geometry from the packed containers
 * (process_expect.inc) and uses these for the auxiliary-bank dirt. */
#define KOS_PCST_SEG_IMAGE  1024u
#define KOS_PCST_SEG_BSS    64u
#define KOS_PCST_SEG_MUL    7u
#define KOS_PCST_SEG_ADD    0x13u
/* The first one-slot auxiliary object of a clean bank map lands in the
 * highest free bank's window-speed slot (4.8 placement policy); the
 * test dirties exactly that slot's BSS to prove the loader zeroes it. */
#define KOS_PCST_SEG_BANK   5u
#define KOS_PCST_SEG_BASE   0x6000u
#define KOS_PCST_BIG1_IMAGE 32760u
#define KOS_PCST_BIG1_BSS   8u
#define KOS_PCST_BIG1_MUL   11u
#define KOS_PCST_BIG1_ADD   0x29u
#define KOS_PCST_BIG2_IMAGE 32760u
#define KOS_PCST_BIG2_BSS   8u
#define KOS_PCST_BIG2_MUL   13u
#define KOS_PCST_BIG2_ADD   0x5Bu

/* Acceptance-store names (systems/subsystem.json). */
#define KOS_PCST_EXETEST_NAME "exetest.exe"
#define KOS_PCST_RETURN_NAME  "exe-return.exe"
/* A deliberately corrupted container and the untouched valid copy:
 * the rejection must leave the allocator and banks clean enough for the
 * valid entry to run in the same boot. */
#define KOS_PCST_TRUNCATED_NAME "exe-truncated.exe"
#define KOS_PCST_VALID_NAME     "exetest.exe"
/* DATA-flagged fixture used by the resource-import checks. */
#define KOS_PCST_DATA_NAME    "exe-wrong-type.dat"

/* Fixture descriptor emitted into the generated process_expect.inc table. */
struct exe_fixture_desc {
	const char *name; /* ROMFS name */
	int code;         /* exact process_exec result */
};

#if KOS_SUBSYS_SELFTEST
void kos_run_process_selftest(void);
#endif

#endif /* KONAOS_TARGET_PROCESS_SELFTEST_H */
