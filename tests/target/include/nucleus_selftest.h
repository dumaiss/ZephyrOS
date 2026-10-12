/* ZephyrOS nucleus-layout acceptance constants (work package 38, test only).
 *
 * Values mirror tests/target/include/nucleus_entry.inc (the assembly view
 * used by tests/target/nucleus/nucleus_entry.asm); the host parity test
 * verifies they cannot drift.  The syscall-side values are additionally
 * pinned to the native ABI headers by static assertions in the driver, so
 * a drifted constant fails the build, not the replay.  Not part of the
 * native ABI.
 */

#ifndef ZEPHYROS_TARGET_NUCLEUS_SELFTEST_H
#define ZEPHYROS_TARGET_NUCLEUS_SELFTEST_H

/* Smoke-caller parameters (application bank 0). */
#define ZOS_NS_APP_BANK      0x00u  /* application bank under test        */
#define ZOS_NS_CALLER_BASE   0x1000u/* blob address in the caller window   */
#define ZOS_NS_TEST_STACK    0x4000u/* blob stack, below the 8000h aperture*/
#define ZOS_NS_ARGBLOCK      0x0800u/* syscall argument block              */

/* Window page the interrupted/calling context has selected (revision 13:
 * 6000h-DFFFh answers bank 7 - P, so P = 1 opens bank 6). */
#define ZOS_NS_PAGE_DRIVER  0x01u

/* SYS_DIAG_SYSTEM_INFO: deterministic, context-free, implemented in the
 * cold half (kernel/syscall/diag.c).  Its 8-byte record is the proof that
 * the syscall gate dispatched with P = 0 and the cold half was visible. */
#define ZOS_NS_SYSCALL_NUM      0x2Du
#define ZOS_NS_SYSCALL_RESULT   8u
#define ZOS_NS_SYSTEM_INFO_SIZE 8u
#define ZOS_NS_SYSTEM_INFO_ABI  1u
#define ZOS_NS_MODE_APPLICATION 0x10u
#define ZOS_NS_MODE_OS          0x18u
#define ZOS_NS_COMMON_LO        0x00u
#define ZOS_NS_COMMON_HI        0xE0u
#define ZOS_NS_KERNEL_BANK      7u
#define ZOS_NS_ROMFS_PAGE       2u

/* Delivery phases the registered handler reports on. */
#define ZOS_NS_PHASE_GATES  1u /* the smoke caller's mode-10 window */
#define ZOS_NS_PHASE_UPCALL 2u /* the upcall shim call (mode 11, P = 1) */

/* Upcall shim exercise (kernel/memory/upcall.h). */
#define ZOS_NS_STUB_BASE     0x7800u/* window address of the bank-6 target */
#define ZOS_NS_STUB_MAGIC    0xA55Au/* the target's HL result              */
#define ZOS_NS_STUB_WITNESS  0x3Cu  /* the stub's witness byte            */
#define ZOS_NS_EINVAL        0x02u
#define ZOS_NS_STUB_STATUS_DELIVERED 0x01u /* the stub took the CTC0 delivery */
#define ZOS_NS_STUB_STATUS_TIMEOUT   0x02u /* it did not arrive in the window  */

/* Result codes recorded in common state by the smoke caller. */
#define ZOS_NS_OK                  0x00u
#define ZOS_NS_FAIL_SYSCALL_CARRY  0x01u
#define ZOS_NS_FAIL_SYSCALL_RESULT 0x02u
#define ZOS_NS_FAIL_SYSCALL_RECORD 0x03u
#define ZOS_NS_FAIL_SYSCALL_LATCH  0x04u
#define ZOS_NS_FAIL_IRQ_TIMEOUT    0x05u
#define ZOS_NS_FAIL_IRQ_LATCH      0x06u
#define ZOS_NS_FAIL_IRQ_HANDLER    0x07u
#define ZOS_NS_FAIL_IRQ_EVIDENCE   0x08u
#define ZOS_NS_FAIL_UPCALL_ENTER   0x09u
#define ZOS_NS_FAIL_UPCALL_RESULT  0x0Au
#define ZOS_NS_FAIL_UPCALL_LATCH   0x0Bu
#define ZOS_NS_FAIL_UPCALL_EINVAL  0x0Cu
#define ZOS_NS_FAIL_UPCALL_RESTORE 0x0Du
#define ZOS_NS_FAIL_UPCALL_IRQ     0x0Eu
#define ZOS_NS_FAIL_UPCALL_SPAN    0x0Fu

/* IRQ handler evidence bits (_zos_nucleus_irq_evidence). */
#define ZOS_NS_EV_DELIVERED 0x01u /* the handler ran at all            */
#define ZOS_NS_EV_PAGE_ZERO 0x02u /* it observed P = 0 (mode 11)       */
#define ZOS_NS_EV_BANK      0x04u /* it observed the interrupted bank  */
#define ZOS_NS_EV_ISR_STACK 0x08u /* it ran on the fixed-core ISR stack*/

#if ZOS_NUCLEUS_SELFTEST
/* Nucleus-layout acceptance entry (tests/target/nucleus/nucleus_test.c). */
void zos_run_nucleus_selftest(void);
#endif

#endif /* ZEPHYROS_TARGET_NUCLEUS_SELFTEST_H */
