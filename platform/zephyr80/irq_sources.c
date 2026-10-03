/* Logical-source to Zephyr-80 hardware mapping. */
#include "zephyros/platform.h"
#include "zephyros/irq.h"

static __sfr __at ZOS_CTC0_PORT zos_ctc0;

uint8_t platform_irq_vector(irq_source_t source)
{
	if (source == IRQ_CTC0) return ZOS_CTC0_VECTOR_BASE;
	if (source == IRQ_VDP_FF) return 0xFFu;
	return 0xFFu;
}

void platform_irq_quiesce(irq_source_t source)
{
	if (source == IRQ_CTC0) zos_ctc0 = ZOS_CTC0_RESET;
}

void platform_ctc0_test_start(void)
{
	zos_ctc0 = ZOS_CTC0_VECTOR_BASE;
	zos_ctc0 = ZOS_CTC0_CONTROL_TEST;
	zos_ctc0 = ZOS_CTC0_TIME_CONSTANT;
}

void platform_ctc0_stop(void)
{
	zos_ctc0 = ZOS_CTC0_RESET;
}
