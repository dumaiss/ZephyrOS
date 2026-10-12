/* KonaOS nucleus-layout acceptance test (test artifact; linked only in
 * acceptance image builds).
 *
 * Runs the revision-13 gate and upcall acceptance surface of work package
 * 38 and reports the outcome on the early console:
 *
 *   1. the smoke caller runs in application bank 0 in mode 10 with the
 *      driver page selected.  It issues SYS_DIAG_SYSTEM_INFO (a cold-half
 *      body), checks the deterministic record it must have written into
 *      the caller's window and the exact restore of the selected latch;
 *   2. still with the driver page selected, it enables interrupts and the
 *      CTC0 delivery is injected.  The cold-half handler records the latch
 *      it observed (P = 0, mode 11, the interrupted bank) and the stack it
 *      ran on (the fixed-core ISR stack); the caller then checks that the
 *      interrupted latch came back byte-exactly;
 *   3. the upcall shim: the fixed-core install helper writes the target
 *      stub into bank 6 through the window page, and this cold-half test
 *      code - the cold-caller case of architecture 36.5 - calls kos_upcall
 *      and checks the selected page, the target's result and argument, and
 *      the exact restore of the latch.
 *
 * Prints `nucleus selftest: OK` only after every check passes.
 */

#include "irq_types.h"
#include "kernel.h"
#include "platform.h"
#include "process.h"

#include "kernel/irq/irq_private.h"
#include "kernel/memory/banking.h"
#include "kernel/memory/upcall.h"

#include "kos/diag.h"

#include "ctc_test.h"
#include "nucleus_selftest.h"

/* The mirror of tests/target/include/nucleus_entry.inc must agree with the
 * native ABI: a drifted constant fails this build, not the replay. */
_Static_assert(KOS_NS_SYSCALL_NUM == KOS_SYSCALL_DIAG_SYSTEM_INFO,
	       "nucleus selftest syscall number differs from the diag ABI");
_Static_assert(KOS_NS_SYSCALL_RESULT == KOS_DIAG_SYSTEM_INFO_SIZE,
	       "nucleus selftest syscall result differs from the diag ABI");
_Static_assert(KOS_NS_SYSTEM_INFO_SIZE == KOS_DIAG_SYSTEM_INFO_SIZE,
	       "nucleus selftest system-info size differs from the diag ABI");
_Static_assert(KOS_NS_SYSTEM_INFO_ABI == KOS_DIAG_SYSTEM_INFO_ABI,
	       "nucleus selftest system-info ABI differs from the diag ABI");
_Static_assert(KOS_NS_MODE_APPLICATION == KOS_MODE_APPLICATION,
	       "nucleus selftest application mode differs from the platform");
_Static_assert(KOS_NS_MODE_OS == KOS_MODE_OS,
	       "nucleus selftest OS mode differs from the platform");
_Static_assert(KOS_NS_KERNEL_BANK == PROCESS_KERNEL_BANK,
	       "nucleus selftest kernel bank differs from the process model");
_Static_assert(KOS_NS_ROMFS_PAGE == KOS_ROMFS_FIRST_PAGE,
	       "nucleus selftest ROMFS page differs from the platform");
_Static_assert(KOS_NS_PAGE_DRIVER == 1u && KOS_UPCALL_PAGE_DRIVER == 1u,
	       "nucleus selftest driver page differs from the upcall shim");

/* Common test state owned by tests/target/nucleus/nucleus_entry.asm. */
extern volatile uint8_t kos_nucleus_result;
extern volatile uint8_t kos_nucleus_irq_evidence;
extern volatile uint8_t kos_nucleus_irq_latch;
extern volatile uint8_t kos_nucleus_phase;
extern volatile uint8_t kos_nucleus_upcall_irq_evidence;
extern volatile uint8_t kos_nucleus_upcall_irq_latch;
extern void kos_nucleus_selftest_run(uint8_t bank);
extern uint8_t kos_nucleus_sp_is_isr(void);

/* Fixed-core upcall state and helpers (same file). */
extern uint8_t kos_nucleus_upcall_latch0;
extern uint16_t kos_nucleus_upcall_arg;
extern uint8_t kos_nucleus_stub_latch;
extern uint16_t kos_nucleus_stub_args;
extern uint8_t kos_nucleus_stub_witness;
extern uint8_t kos_nucleus_stub_status;
extern uint8_t kos_nucleus_stub_latch_after;
extern uint8_t kos_nucleus_stub_install(void);

static const char kos_nucleus_hex_digits[] = "0123456789ABCDEF";

static void nucleus_selftest_report_fail(const char *phase, uint8_t code)
{
	early_console_puts("nucleus selftest: FAIL ");
	early_console_puts(phase);
	early_console_puts(" code=0x");
	early_console_putc(kos_nucleus_hex_digits[code >> 4]);
	early_console_putc(kos_nucleus_hex_digits[code & 0x0F]);
	early_console_puts("\r\n");
}

/* Registered CTC0 handler.  Runs in mode 11 on the fixed-core ISR stack
 * with the gate-normalized latch, and records compact evidence only: the
 * latch it observed and where it ran.  It is cold-half code itself, so a
 * gate that left the page field set could not reach it at all. */
static void nucleus_selftest_handler(void *context)
{
	uint8_t evidence = KOS_NS_EV_DELIVERED;
	uint8_t latch = platform_memctl_read();

	(void)context;
	if ((latch & KOS_MODE_MASK) == KOS_MODE_OS) {
		evidence |= KOS_NS_EV_PAGE_ZERO;
	}
	if ((latch & KOS_MODE_MASK) == KOS_MODE_OS &&
	    (latch & KOS_BANK_MASK) == KOS_NS_APP_BANK) {
		evidence |= KOS_NS_EV_BANK;
	}
	if (kos_nucleus_sp_is_isr()) {
		evidence |= KOS_NS_EV_ISR_STACK;
	}
	if (kos_nucleus_phase == KOS_NS_PHASE_UPCALL) {
		kos_nucleus_upcall_irq_latch = latch;
		kos_nucleus_upcall_irq_evidence |= evidence;
	} else {
		kos_nucleus_irq_latch = latch;
		kos_nucleus_irq_evidence |= evidence;
	}
}

/* The cold-caller case of architecture 36.5: this code runs in the cold
 * half, so it cannot execute while the window shows bank 6 - the fixed
 * shim makes the call instead. */
static uint8_t nucleus_selftest_upcall(void)
{
	struct kos_upcall_req req;
	uint8_t before;
	uint8_t expected;

	/* Common state is linked image content, not zeroed RAM: reset the
	 * evidence this phase reads before it runs. */
	kos_nucleus_upcall_irq_evidence = 0;
	kos_nucleus_upcall_irq_latch = 0;
	kos_nucleus_stub_witness = 0;
	kos_nucleus_stub_status = 0;
	kos_nucleus_stub_latch = 0;
	kos_nucleus_stub_latch_after = 0;
	if (kos_nucleus_stub_install() != 0) {
		return KOS_NS_FAIL_UPCALL_ENTER;
	}
	before = platform_memctl_read();
	expected = (uint8_t)((before & (KOS_MODE_MASK | KOS_BANK_MASK)) |
			     (KOS_NS_PAGE_DRIVER << KOS_ROM_PAGE_SHIFT));

	/* Rejected requests must not touch the latch. */
	req.page = KOS_NS_PAGE_DRIVER + 1u;
	req.entry = KOS_NS_STUB_BASE;
	req.args = (void *)0;
	req.result = 0;
	if (kos_upcall(&req) != KOS_NS_EINVAL) {
		return KOS_NS_FAIL_UPCALL_EINVAL;
	}
	req.page = KOS_NS_PAGE_DRIVER;
	req.entry = 0x5000u; /* the fixed core: outside the window */
	if (kos_upcall(&req) != KOS_NS_EINVAL) {
		return KOS_NS_FAIL_UPCALL_EINVAL;
	}
	if (platform_memctl_read() != before) {
		return KOS_NS_FAIL_UPCALL_RESTORE;
	}

	/* The upcall itself: select page 1, call the bank-6 stub, restore.
	 * The stub is interrupted by CTC0 while it runs - mode 11 with the
	 * window on bank 6 - so this is also the case the IRQ gate has to
	 * normalize: the cold-half handler must be reachable and the stub
	 * must find its page still selected when it resumes. */
	req.entry = KOS_NS_STUB_BASE;
	req.args = (void *)&kos_nucleus_upcall_arg;
	req.result = 0;
	if (kos_upcall(&req) != 0) {
		return KOS_NS_FAIL_UPCALL_ENTER;
	}
	if (req.result != KOS_NS_STUB_MAGIC ||
	    kos_nucleus_stub_witness != KOS_NS_STUB_WITNESS ||
	    kos_nucleus_stub_args != (uint16_t)&kos_nucleus_upcall_arg) {
		return KOS_NS_FAIL_UPCALL_RESULT;
	}
	if (kos_nucleus_stub_latch != expected) {
		return KOS_NS_FAIL_UPCALL_LATCH;
	}
	if (kos_nucleus_stub_status != KOS_NS_STUB_STATUS_DELIVERED ||
	    kos_nucleus_upcall_irq_evidence !=
		    (KOS_NS_EV_DELIVERED | KOS_NS_EV_PAGE_ZERO | KOS_NS_EV_BANK |
		     KOS_NS_EV_ISR_STACK) ||
	    kos_nucleus_upcall_irq_latch !=
		    (uint8_t)(KOS_MODE_OS | KOS_NS_APP_BANK)) {
		return KOS_NS_FAIL_UPCALL_IRQ;
	}
	if (kos_nucleus_stub_latch_after != expected) {
		return KOS_NS_FAIL_UPCALL_SPAN;
	}
	if (platform_memctl_read() != before) {
		return KOS_NS_FAIL_UPCALL_RESTORE;
	}
	return KOS_NS_OK;
}

/* Runs the smoke caller in application bank 0 (syscall and IRQ gates with
 * the driver page selected), then the upcall shim from the cold half,
 * including the CTC0 delivery that arrives while bank 6 is mapped. */
void kos_run_nucleus_selftest(void)
{
	uint8_t code;

	if (irq_claim(IRQ_CTC0, nucleus_selftest_handler, (void *)0) != IRQ_OK) {
		nucleus_selftest_report_fail("claim", 0xFFu);
		return;
	}
	kos_nucleus_result = 0xFFu;
	/* CTC0 stays programmed across both phases: the delivery arrives in
	 * the smoke caller's mode-10 window and again while the upcall shim
	 * has bank 6 mapped. */
	kos_ctc0_test_start();
	kos_nucleus_selftest_run(KOS_NS_APP_BANK);

	code = kos_nucleus_result;
	if (code != KOS_NS_OK) {
		kos_ctc0_test_stop();
		(void)irq_release(IRQ_CTC0);
		nucleus_selftest_report_fail("gates", code);
		return;
	}
	code = nucleus_selftest_upcall();
	kos_ctc0_test_stop();
	(void)irq_release(IRQ_CTC0);
	if (code != KOS_NS_OK) {
		nucleus_selftest_report_fail("upcall", code);
		return;
	}

	early_console_puts("nucleus selftest: OK\r\n");
}
