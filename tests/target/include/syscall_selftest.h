/* KonaOS syscall-gate acceptance constants (test only).
 *
 * Values mirror tests/target/include/syscall_entry.inc (the assembly view
 * used by tests/target/syscall/syscall_entry.asm); the host parity test
 * verifies they cannot drift.  Not part of the native ABI.
 */

#ifndef KONAOS_TARGET_SYSCALL_SELFTEST_H
#define KONAOS_TARGET_SYSCALL_SELFTEST_H

/* Smoke-caller parameters. */
#define KOS_ST_CALLER_BASE   0x1000u  /* caller code address (window)     */
#define KOS_ST_TEST_STACK    0x4000u  /* caller stack, above 2000h        */
#define KOS_ST_SYSCALL_NUM   0x55u    /* any number; all return ENOSYS    */
#define KOS_ST_ARGBLOCK      0x0800u  /* dummy arg block in caller window */

/* Sentinel values for preserved registers. */
#define KOS_ST_BC_SENTINEL   0xB0BCu
#define KOS_ST_DE_SENTINEL   0xD0DEu
#define KOS_ST_IX_SENTINEL   0x19A0u
#define KOS_ST_IY_SENTINEL   0x1BA1u

/* Result codes recorded in common state by the smoke caller. */
#define KOS_ST_OK            0x00u
#define KOS_ST_FAIL_CARRY    0x01u
#define KOS_ST_FAIL_ERRNO    0x02u
#define KOS_ST_FAIL_RESULT   0x03u
#define KOS_ST_FAIL_BC       0x04u
#define KOS_ST_FAIL_DE       0x05u
#define KOS_ST_FAIL_IX       0x06u
#define KOS_ST_FAIL_IY       0x07u
#define KOS_ST_FAIL_SP       0x08u
#define KOS_ST_FAIL_MODE     0x09u
#define KOS_ST_FAIL_BANK     0x0Au

#if KOS_SUBSYS_SELFTEST
/* Syscall acceptance entry (tests/target/syscall/syscall_test.c). */
void kos_run_syscall_selftest(void);
#endif

#endif /* KONAOS_TARGET_SYSCALL_SELFTEST_H */
