/* Production CTC owner: the logical-source to Zephyr-80 hardware
 * quiesce mapping.
 *
 * Only platform_irq_quiesce() is a production consumer.  The CTC0
 * start/stop programming and the obsolete vector lookup are
 * acceptance-only and live under tests/target/ (tests/target/include/
 * ctc_test.h, tests/target/support/ctc_test.c); no production image
 * declares or links them.
 */

#include "zephyr80/hardware.h"

#include "kernel/irq/irq_private.h"

static __sfr __at ZOS_CTC0_PORT zos_ctc0;

void platform_irq_quiesce(irq_source_t source)
{
	if (source == IRQ_CTC0) zos_ctc0 = ZOS_CTC0_RESET;
}
