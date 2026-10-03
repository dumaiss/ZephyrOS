/* ZephyrOS M1 syscall-gate self-test constants (C visible).
 *
 * Values mirror platform/zephyr80/selftest.inc; the host parity test
 * verifies they cannot drift.  Test-only; not part of the native ABI.
 */

#ifndef ZEPHYROS_SELFTEST_H
#define ZEPHYROS_SELFTEST_H

/* Smoke-caller parameters. */
#define ZOS_ST_CALLER_BASE   0x1000u  /* caller code address (window)     */
#define ZOS_ST_TEST_STACK    0x4000u  /* caller stack, above 2000h        */
#define ZOS_ST_SYSCALL_NUM   0x55u    /* any number; all return ENOSYS    */
#define ZOS_ST_ARGBLOCK      0x0800u  /* dummy arg block in caller window */

/* Sentinel values for preserved registers. */
#define ZOS_ST_BC_SENTINEL   0xB0BCu
#define ZOS_ST_DE_SENTINEL   0xD0DEu
#define ZOS_ST_IX_SENTINEL   0x19A0u
#define ZOS_ST_IY_SENTINEL   0x1BA1u

/* Result codes recorded in common state by the smoke caller. */
#define ZOS_ST_OK            0x00u
#define ZOS_ST_FAIL_CARRY    0x01u
#define ZOS_ST_FAIL_ERRNO    0x02u
#define ZOS_ST_FAIL_RESULT   0x03u
#define ZOS_ST_FAIL_BC       0x04u
#define ZOS_ST_FAIL_DE       0x05u
#define ZOS_ST_FAIL_IX       0x06u
#define ZOS_ST_FAIL_IY       0x07u
#define ZOS_ST_FAIL_SP       0x08u
#define ZOS_ST_FAIL_MODE     0x09u
#define ZOS_ST_FAIL_BANK     0x0Au

#endif /* ZEPHYROS_SELFTEST_H */
