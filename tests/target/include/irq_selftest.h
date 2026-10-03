/* ZephyrOS M2 IRQ acceptance constants (test only).
 *
 * Values mirror tests/target/include/irq_selftest.inc (the assembly view
 * used by tests/target/m2/irq_selftest.asm); the host parity test verifies
 * they cannot drift.  Not part of the native ABI.
 */

#ifndef ZEPHYROS_TARGET_M2_SELFTEST_H
#define ZEPHYROS_TARGET_M2_SELFTEST_H

/* Smoke-caller parameters. */
#define ZOS_IST_CALLER_BASE   0x1000u  /* caller code address (window)     */
#define ZOS_IST_TEST_STACK    0x4000u  /* caller stack, above 2000h        */
#define ZOS_IST_SYSCALL_NUM   0x55u    /* any number; all return ENOSYS    */
#define ZOS_IST_ARGBLOCK      0x0800u  /* dummy arg block in caller window */

/* Sentinel values for preserved registers. */
#define ZOS_IST_BC_SENTINEL   0xB1C2u
#define ZOS_IST_DE_SENTINEL   0xD3E4u
#define ZOS_IST_HL_SENTINEL   0xA5F6u
#define ZOS_IST_IX_SENTINEL   0x1978u
#define ZOS_IST_IY_SENTINEL   0x1B9Au
#define ZOS_IST_A_SENTINEL    0x55u

#define ZOS_IST_ALT_A_SENTINEL   0xAAu
#define ZOS_IST_ALT_BC_SENTINEL  0x1234u
#define ZOS_IST_ALT_DE_SENTINEL  0x5678u
#define ZOS_IST_ALT_HL_SENTINEL  0x9ABCu

/* Result code stored in common state by the smoke caller before the
 * driver runs; the driver arms the cell with this sentinel first. */
#define ZOS_IST_RESULT_ARMED   0xFFu

#define ZOS_IST_OK              0x00u
#define ZOS_IST_FAIL_TIMEOUT    0x01u
#define ZOS_IST_FAIL_REPEAT     0x02u
#define ZOS_IST_FAIL_BC         0x03u
#define ZOS_IST_FAIL_DE         0x04u
#define ZOS_IST_FAIL_HL         0x05u
#define ZOS_IST_FAIL_IX         0x06u
#define ZOS_IST_FAIL_IY         0x07u
#define ZOS_IST_FAIL_AF         0x08u
#define ZOS_IST_FAIL_SP         0x09u
#define ZOS_IST_FAIL_MODE       0x0Au
#define ZOS_IST_FAIL_BANK       0x0Bu
#define ZOS_IST_FAIL_ALT_AF     0x0Cu
#define ZOS_IST_FAIL_ALT_BC     0x0Du
#define ZOS_IST_FAIL_ALT_DE     0x0Eu
#define ZOS_IST_FAIL_ALT_HL     0x0Fu
#define ZOS_IST_FAIL_IFF        0x10u
#define ZOS_IST_FAIL_SYSCALL    0x11u
#define ZOS_IST_FAIL_EVIDENCE   0x12u
#define ZOS_IST_FAIL_COUNTERS   0x13u
#define ZOS_IST_FAIL_UNCLAIMED  0x14u
#define ZOS_IST_FAIL_REGISTRY   0x15u

#if ZOS_IRQ_SELFTEST
/* M2 acceptance entry (tests/target/m2/irq_test.c). */
void zos_run_irq_selftest(void);
#endif

#endif /* ZEPHYROS_TARGET_M2_SELFTEST_H */
