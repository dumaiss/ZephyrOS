/* ZephyrOS nucleus-layout acceptance test (test artifact; linked only in
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
 *      code - the cold-caller case of architecture 36.5 - calls zos_upcall
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

#include "zos/diag.h"

#include "ctc_test.h"
#include "nucleus_selftest.h"

/* The mirror of tests/target/include/nucleus_entry.inc must agree with the
 * native ABI: a drifted constant fails this build, not the replay. */
_Static_assert(ZOS_NS_SYSCALL_NUM == ZOS_SYSCALL_DIAG_SYSTEM_INFO,
	       "nucleus selftest syscall number differs from the diag ABI");
_Static_assert(ZOS_NS_SYSCALL_RESULT == ZOS_DIAG_SYSTEM_INFO_SIZE,
	       "nucleus selftest syscall result differs from the diag ABI");
_Static_assert(ZOS_NS_SYSTEM_INFO_SIZE == ZOS_DIAG_SYSTEM_INFO_SIZE,
	       "nucleus selftest system-info size differs from the diag ABI");
_Static_assert(ZOS_NS_SYSTEM_INFO_ABI == ZOS_DIAG_SYSTEM_INFO_ABI,
	       "nucleus selftest system-info ABI differs from the diag ABI");
_Static_assert(ZOS_NS_MODE_APPLICATION == ZOS_MODE_APPLICATION,
	       "nucleus selftest application mode differs from the platform");
_Static_assert(ZOS_NS_MODE_OS == ZOS_MODE_OS,
	       "nucleus selftest OS mode differs from the platform");
_Static_assert(ZOS_NS_KERNEL_BANK == PROCESS_KERNEL_BANK,
	       "nucleus selftest kernel bank differs from the process model");
_Static_assert(ZOS_NS_ROMFS_PAGE == ZOS_ROMFS_FIRST_PAGE,
	       "nucleus selftest ROMFS page differs from the platform");
_Static_assert(ZOS_NS_PAGE_DRIVER == 1u && ZOS_UPCALL_PAGE_DRIVER == 1u,
	       "nucleus selftest driver page differs from the upcall shim");

/* Common test state owned by tests/target/nucleus/nucleus_entry.asm. */
extern volatile uint8_t zos_nucleus_result;
extern volatile uint8_t zos_nucleus_irq_evidence;
extern volatile uint8_t zos_nucleus_irq_latch;
extern volatile uint8_t zos_nucleus_phase;
extern volatile uint8_t zos_nucleus_upcall_irq_evidence;
extern volatile uint8_t zos_nucleus_upcall_irq_latch;
extern void zos_nucleus_selftest_run(uint8_t bank);
extern uint8_t zos_nucleus_sp_is_isr(void);

/* Fixed-core upcall state and helpers (same file). */
extern uint8_t zos_nucleus_upcall_latch0;
extern uint16_t zos_nucleus_upcall_arg;
extern uint8_t zos_nucleus_stub_latch;
extern uint16_t zos_nucleus_stub_args;
extern uint8_t zos_nucleus_stub_witness;
extern uint8_t zos_nucleus_stub_status;
extern uint8_t zos_nucleus_stub_latch_after;
extern uint8_t zos_nucleus_stub_install(void);

static const char zos_nucleus_hex_digits[] = "0123456789ABCDEF";

static void nucleus_selftest_report_fail(const char *phase, uint8_t code)
{
	early_console_puts("nucleus selftest: FAIL ");
	early_console_puts(phase);
	early_console_puts(" code=0x");
	early_console_putc(zos_nucleus_hex_digits[code >> 4]);
	early_console_putc(zos_nucleus_hex_digits[code & 0x0F]);
	early_console_puts("\r\n");
}

/* Registered CTC0 handler.  Runs in mode 11 on the fixed-core ISR stack
 * with the gate-normalized latch, and records compact evidence only: the
 * latch it observed and where it ran.  It is cold-half code itself, so a
 * gate that left the page field set could not reach it at all. */
static void nucleus_selftest_handler(void *context)
{
	uint8_t evidence = ZOS_NS_EV_DELIVERED;
	uint8_t latch = platform_memctl_read();

	(void)context;
	if ((latch & ZOS_MODE_MASK) == ZOS_MODE_OS) {
		evidence |= ZOS_NS_EV_PAGE_ZERO;
	}
	if ((latch & ZOS_MODE_MASK) == ZOS_MODE_OS &&
	    (latch & ZOS_BANK_MASK) == ZOS_NS_APP_BANK) {
		evidence |= ZOS_NS_EV_BANK;
	}
	if (zos_nucleus_sp_is_isr()) {
		evidence |= ZOS_NS_EV_ISR_STACK;
	}
	if (zos_nucleus_phase == ZOS_NS_PHASE_UPCALL) {
		zos_nucleus_upcall_irq_latch = latch;
		zos_nucleus_upcall_irq_evidence |= evidence;
	} else {
		zos_nucleus_irq_latch = latch;
		zos_nucleus_irq_evidence |= evidence;
	}
}

/* The cold-caller case of architecture 36.5: this code runs in the cold
 * half, so it cannot execute while the window shows bank 6 - the fixed
 * shim makes the call instead. */
static uint8_t nucleus_selftest_upcall(void)
{
	struct zos_upcall_req req;
	uint8_t before;
	uint8_t expected;

	/* Common state is linked image content, not zeroed RAM: reset the
	 * evidence this phase reads before it runs. */
	zos_nucleus_upcall_irq_evidence = 0;
	zos_nucleus_upcall_irq_latch = 0;
	zos_nucleus_stub_witness = 0;
	zos_nucleus_stub_status = 0;
	zos_nucleus_stub_latch = 0;
	zos_nucleus_stub_latch_after = 0;
	if (zos_nucleus_stub_install() != 0) {
		return ZOS_NS_FAIL_UPCALL_ENTER;
	}
	before = platform_memctl_read();
	expected = (uint8_t)((before & (ZOS_MODE_MASK | ZOS_BANK_MASK)) |
			     (ZOS_NS_PAGE_DRIVER << ZOS_ROM_PAGE_SHIFT));

	/* Rejected requests must not touch the latch. */
	req.page = ZOS_NS_PAGE_DRIVER + 1u;
	req.entry = ZOS_NS_STUB_BASE;
	req.args = (void *)0;
	req.result = 0;
	if (zos_upcall(&req) != ZOS_NS_EINVAL) {
		return ZOS_NS_FAIL_UPCALL_EINVAL;
	}
	req.page = ZOS_NS_PAGE_DRIVER;
	req.entry = 0x5000u; /* the fixed core: outside the window */
	if (zos_upcall(&req) != ZOS_NS_EINVAL) {
		return ZOS_NS_FAIL_UPCALL_EINVAL;
	}
	if (platform_memctl_read() != before) {
		return ZOS_NS_FAIL_UPCALL_RESTORE;
	}

	/* The upcall itself: select page 1, call the bank-6 stub, restore.
	 * The stub is interrupted by CTC0 while it runs - mode 11 with the
	 * window on bank 6 - so this is also the case the IRQ gate has to
	 * normalize: the cold-half handler must be reachable and the stub
	 * must find its page still selected when it resumes. */
	req.entry = ZOS_NS_STUB_BASE;
	req.args = (void *)&zos_nucleus_upcall_arg;
	req.result = 0;
	if (zos_upcall(&req) != 0) {
		return ZOS_NS_FAIL_UPCALL_ENTER;
	}
	if (req.result != ZOS_NS_STUB_MAGIC ||
	    zos_nucleus_stub_witness != ZOS_NS_STUB_WITNESS ||
	    zos_nucleus_stub_args != (uint16_t)&zos_nucleus_upcall_arg) {
		return ZOS_NS_FAIL_UPCALL_RESULT;
	}
	if (zos_nucleus_stub_latch != expected) {
		return ZOS_NS_FAIL_UPCALL_LATCH;
	}
	if (zos_nucleus_stub_status != ZOS_NS_STUB_STATUS_DELIVERED ||
	    zos_nucleus_upcall_irq_evidence !=
		    (ZOS_NS_EV_DELIVERED | ZOS_NS_EV_PAGE_ZERO | ZOS_NS_EV_BANK |
		     ZOS_NS_EV_ISR_STACK) ||
	    zos_nucleus_upcall_irq_latch !=
		    (uint8_t)(ZOS_MODE_OS | ZOS_NS_APP_BANK)) {
		return ZOS_NS_FAIL_UPCALL_IRQ;
	}
	if (zos_nucleus_stub_latch_after != expected) {
		return ZOS_NS_FAIL_UPCALL_SPAN;
	}
	if (platform_memctl_read() != before) {
		return ZOS_NS_FAIL_UPCALL_RESTORE;
	}
	return ZOS_NS_OK;
}

/* Runs the smoke caller in application bank 0 (syscall and IRQ gates with
 * the driver page selected), then the upcall shim from the cold half,
 * including the CTC0 delivery that arrives while bank 6 is mapped. */
void zos_run_nucleus_selftest(void)
{
	uint8_t code;

	if (irq_claim(IRQ_CTC0, nucleus_selftest_handler, (void *)0) != IRQ_OK) {
		nucleus_selftest_report_fail("claim", 0xFFu);
		return;
	}
	zos_nucleus_result = 0xFFu;
	/* CTC0 stays programmed across both phases: the delivery arrives in
	 * the smoke caller's mode-10 window and again while the upcall shim
	 * has bank 6 mapped. */
	zos_ctc0_test_start();
	zos_nucleus_selftest_run(ZOS_NS_APP_BANK);

	code = zos_nucleus_result;
	if (code != ZOS_NS_OK) {
		zos_ctc0_test_stop();
		(void)irq_release(IRQ_CTC0);
		nucleus_selftest_report_fail("gates", code);
		return;
	}
	code = nucleus_selftest_upcall();
	zos_ctc0_test_stop();
	(void)irq_release(IRQ_CTC0);
	if (code != ZOS_NS_OK) {
		nucleus_selftest_report_fail("upcall", code);
		return;
	}

	early_console_puts("nucleus selftest: OK\r\n");
}
