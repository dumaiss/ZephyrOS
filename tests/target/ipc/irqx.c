/* ZephyrOS IPC acceptance: the IRQ-owning synthetic service (test
 * artifact).
 *
 * Proves that a resident service can own a hardware IRQ without owning the
 * vectoring: the service claims CTC0 through the nucleus API (at init and
 * through the CLAIM op, which also lets the client exercise RELEASE) and
 * hands it a handler that lives in bank 6.  The nucleus keeps the IM2
 * page, the transition code, the ISR stack and the final RETI; the service
 * handler runs in the service domain on the nucleus ISR stack with the
 * restricted API (no IPC).
 *
 * SPIN arms the timer, enables interrupts for the duration of the spin
 * (the syscall gate runs with interrupts masked, and the gate restores the
 * caller's interrupt state on return), waits for the handler's flag and
 * masks them again.  The delivery therefore happens while a service call
 * is in progress, which is the case that must not disturb the call's saved
 * latch (architecture 36.10).
 *
 * COPY (correction 01 to architecture 36.8) is the same idea one step
 * further in: it repeats a descriptor copy into this service's own I/O
 * buffer with the timer armed on a short time constant, so the delivery
 * arrives while the copy primitive holds its mode-10 aperture window open
 * (path B).  The IRQ gate must restore that exact latch - mode, Q and
 * P = 1 - and the copy must finish with the bytes intact.
 */

#include "zephyros/errno.h"
#include "zephyros/ipc.h"

#include "zephyr80/hardware.h"
#include "zephyr80/irq.h"

#include "ipc_test.h"

static const struct ipc_service_api *api;
static volatile uint16_t irq_runs;
static volatile uint8_t irq_flag;

/* The copy target: an ordinary container static, so the loader places it
 * inside bank 6's 8000h-BFFFh (the service I/O buffer rule, correction 01
 * to architecture 36.8). */
static uint8_t irqx_buffer[IRQX_COPY_BYTES];
static uint16_t irqx_buffer_address;

/* The test-owned CTC0 programming sequence (vector base, control word,
 * time constant) and its reset, mirroring tests/target/support/ctc_test.c.
 * A real timer service would own this programming; here it is the
 * synthetic IRQ source the suite needs. */
static __sfr __at ZOS_CTC0_PORT ipc_ctc0;

static void irqx_timer_start(void)
{
	ipc_ctc0 = ZOS_CTC0_VECTOR_BASE;
	ipc_ctc0 = ZOS_CTC0_CONTROL_TEST;
	ipc_ctc0 = ZOS_CTC0_TIME_CONSTANT;
}

static void irqx_timer_stop(void)
{
	ipc_ctc0 = ZOS_CTC0_RESET;
}

/* The correction profile: same vector and control word, short time
 * constant, so the modelled timer period fits inside one aperture
 * window. */
static void irqx_timer_start_short(void)
{
	ipc_ctc0 = ZOS_CTC0_VECTOR_BASE;
	ipc_ctc0 = ZOS_CTC0_CONTROL_TEST;
	ipc_ctc0 = IPC_IRQX_TC_APERTURE;
}

/* Runs from the IRQ executive, in the service domain, on the nucleus ISR
 * stack.  Restricted API only: no IPC, no descriptor copies. */
static void irqx_handler(void *context)
{
	(void)context;
	irq_runs = (uint16_t)(irq_runs + 1u);
	irq_flag = 0xA5u;
}

static int16_t irqx_spin(struct zos_ipc_frame *frame)
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

/* Repeat a descriptor copy into the service buffer with the timer armed,
 * so a delivery lands inside a path-B aperture window.  The caller's
 * buffer is the frame's descriptor (direction IN); the reply carries a
 * 16-bit sum of what landed in the service buffer, the deliveries and the
 * completed copies, and the delivery counter is cleared afterwards so the
 * client's absolute counts start from zero. */
static int16_t irqx_copies(struct zos_ipc_frame *frame)
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
			return -ZOS_EINVAL;
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

static int16_t irqx_dispatch(struct zos_ipc_frame *frame)
{
	switch (frame->op) {
	case IPC_OP_IRQX_COPY:
		return irqx_copies(frame);
	case IPC_OP_IRQX_SPIN:
		return irqx_spin(frame);
	case IPC_OP_IRQX_CLAIM:
		return api->irq_claim(IPC_IRQ_CTC0, (uint16_t)irqx_handler, 0u);
	case IPC_OP_IRQX_RELEASE:
		return api->irq_release(IPC_IRQ_CTC0);
	default:
		return -ZOS_EINVAL;
	}
}

void ipc_irqx_init(const struct ipc_service_api *table)
{
	api = table;
	irqx_buffer_address = (uint16_t)irqx_buffer;
	(void)table->register_ep(IPC_CLASS_IRQX, 0u, (uint16_t)irqx_dispatch,
				 0u, 1u);
	(void)table->irq_claim(IPC_IRQ_CTC0, (uint16_t)irqx_handler, 0u);
}
