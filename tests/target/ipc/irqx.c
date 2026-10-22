/* KonaOS IPC acceptance: the IRQ-owning synthetic driver (test
 * artifact).
 *
 * Proves that a resident driver can own a hardware IRQ without owning the
 * vectoring: the driver claims CTC0 through the nucleus API (at init and
 * through the CLAIM op, which also lets the client exercise RELEASE) and
 * hands it a handler that lives in bank 6.  The nucleus keeps the IM2
 * page, the transition code, the ISR stack and the final RETI; the driver
 * handler runs in the driver domain on the nucleus ISR stack with the
 * restricted API (no IPC).
 *
 * SPIN arms the timer, enables interrupts for the duration of the spin
 * (the syscall gate runs with interrupts masked, and the gate restores the
 * caller's interrupt state on return), waits for the handler's flag and
 * masks them again.  The delivery therefore happens while a driver call
 * is in progress, which is the case that must not disturb the call's saved
 * latch (architecture 4.5).
 *
 * COPY (architecture 4.9) is the same idea one step
 * further in: it repeats a descriptor copy into this driver's own I/O
 * buffer with the timer armed on a short time constant, so the delivery
 * arrives while the copy primitive holds its mode-10 aperture window open
 * (path B).  The IRQ gate must restore that exact latch - mode, Q and
 * P = 1 - and the copy must finish with the bytes intact.
 */

#include "kos/errno.h"
#include "driver.h"

#include "hardware.h"
#include "irq.h"

#include "ipc_test.h"

static const struct driver_api *api;
static volatile uint16_t irq_runs;
static volatile uint8_t irq_flag;

/* The copy target: an ordinary container static, so the loader places it
 * inside bank 6's 8000h-BFFFh (the driver I/O buffer rule, architecture
 * 4.9). */
static uint8_t irqx_buffer[IRQX_COPY_BYTES];
static uint16_t irqx_buffer_address;

/* The test-owned CTC0 programming sequence (vector base, control word,
 * time constant) and its reset, mirroring tests/target/support/ctc_test.c.
 * A real timer driver would own this programming; here it is the
 * synthetic IRQ source the suite needs. */
static __sfr __at KOS_CTC0_PORT ipc_ctc0;

static void irqx_timer_start(void)
{
	ipc_ctc0 = KOS_CTC0_VECTOR_BASE;
	ipc_ctc0 = KOS_CTC0_CONTROL_TEST;
	ipc_ctc0 = KOS_CTC0_TIME_CONSTANT;
}

static void irqx_timer_stop(void)
{
	ipc_ctc0 = KOS_CTC_RESET;
}

/* The correction profile: same vector and control word, short time
 * constant, so the modelled timer period fits inside one aperture
 * window. */
static void irqx_timer_start_short(void)
{
	ipc_ctc0 = KOS_CTC0_VECTOR_BASE;
	ipc_ctc0 = KOS_CTC0_CONTROL_TEST;
	ipc_ctc0 = IPC_IRQX_TC_APERTURE;
}

/* Runs from the IRQ executive, in the driver domain, on the nucleus ISR
 * stack.  Restricted API only: no IPC, no descriptor copies. */
static void irqx_handler(void *context)
{
	(void)context;
	irq_runs = (uint16_t)(irq_runs + 1u);
	irq_flag = 0xA5u;
}

static int16_t irqx_spin(struct kos_ipc_frame *frame)
{
	uint16_t iterations = frame->arg1;

	irq_flag = 0u;
	irqx_timer_start();
	__asm
		ei
	__endasm;
	while (iterations != 0u) {
		if (irq_flag != 0u) {
			break;
		}
		--iterations;
	}
	__asm
		di
	__endasm;
	irqx_timer_stop();
	return (int16_t)irq_runs;
}

/* Repeat a descriptor copy into the driver buffer with the timer armed,
 * so a delivery lands inside a path-B aperture window.  The caller's
 * buffer is the frame's descriptor (direction IN); the reply carries a
 * 16-bit sum of what landed in the driver buffer, the deliveries and the
 * completed copies, and the delivery counter is cleared afterwards so the
 * client's absolute counts start from zero. */
static int16_t irqx_copies(struct kos_ipc_frame *frame)
{
	uint16_t iterations = frame->arg1;
	uint8_t copies = 0u;
	uint16_t index;
	uint16_t sum = 0u;

	irq_flag = 0u;
	irq_runs = 0u;
	irqx_timer_start_short();
	__asm
		ei
	__endasm;
	while (iterations != 0u) {
		if (api->copy_in(frame->desc, 0u, irqx_buffer_address,
				 (uint16_t)IRQX_COPY_BYTES) < 0) {
			__asm
				di
			__endasm;
			irqx_timer_stop();
			irq_runs = 0u;
			return -KOS_EINVAL;
		}
		++copies;
		--iterations;
	}
	__asm
		di
	__endasm;
	irqx_timer_stop();
	for (index = 0u; index < IRQX_COPY_BYTES; ++index) {
		sum = (uint16_t)(sum + irqx_buffer[index]);
	}
	frame->payload[0] = IPC_WORD_LO(sum);
	frame->payload[1] = IPC_WORD_HI(sum);
	frame->payload[2] = (uint8_t)irq_runs;
	frame->payload[3] = copies;
	irq_runs = 0u;
	return 0;
}

extern const uint8_t irqx_mapped[IPC_SPAN_BYTES];
static __sfr __at 0x00 irqx_latch;
static const uint8_t *irqx_pattern; /* set at init (echo.c's note) */

/* IPC_OP_IRQX_SPAN (ipc_test.h): deliveries inside a bulk-map span return
 * to it (architecture 4.5), so the pattern still reads back afterwards. */
static int16_t irqx_span(struct kos_ipc_frame *frame)
{
	uint16_t iterations = frame->arg1;
	uint8_t runs = (uint8_t)irq_runs;
	uint8_t ok = 1u;
	uint8_t i;
	const uint8_t *pattern;

	if (api->bulk_map() != 0) {
		return -KOS_EIO;
	}
	irq_flag = 0u;
	irqx_timer_start();
	__asm
		ei
	__endasm;
	while (iterations != 0u && irq_flag == 0u) {
		--iterations;
	}
	__asm
		di
	__endasm;
	irqx_timer_stop();
	if ((irqx_latch & 0x07u) != 6u) {
		ok = 0u;
	}
	pattern = irqx_pattern;
	for (i = 0u; i < IPC_SPAN_BYTES; ++i) {
		if (*pattern++ != (uint8_t)(0xC0u + i)) {
			ok = 0u;
		}
	}
	api->bulk_unmap();
	if ((uint8_t)irq_runs == runs) {
		return -KOS_ETIMEDOUT;
	}
	return ok ? (int16_t)IPC_SPAN_IRQ_OK : -KOS_EIO;
}

/* The endpoint dispatch entry the manifest names: a module-global
 * symbol so the packer can resolve its hot-image offset. */
int16_t irqx_dispatch(struct kos_ipc_frame *frame)
{
	switch (frame->op) {
	case IPC_OP_IRQX_COPY:
		return irqx_copies(frame);
	case IPC_OP_IRQX_SPIN:
		return irqx_spin(frame);
	case IPC_OP_IRQX_SPAN:
		return irqx_span(frame);
	case IPC_OP_IRQX_CLAIM:
		return api->irq_claim(IPC_IRQ_CTC0, (uint16_t)irqx_handler, 0u);
	case IPC_OP_IRQX_RELEASE:
		return api->irq_release(IPC_IRQ_CTC0);
	default:
		return -KOS_EINVAL;
	}
}

uint8_t ipc_irqx_init(const struct driver_boot *boot)
{
	api = boot->api;
	irqx_buffer_address = (uint16_t)irqx_buffer;
	irqx_pattern = irqx_mapped;
	/* The endpoint is installed by the resident loader from the image
	 * metadata; the IRQ claim is the driver's own bring-up step, run
	 * here in the mode-11 window where the fixed core (and so the
	 * bootstrap interface) is visible. */
	(void)boot->api->irq_claim(IPC_IRQ_CTC0, (uint16_t)irqx_handler, 0u);
	return 0u;
}
