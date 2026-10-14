/* Production CTC owner: the logical-source to Zephyr-80 hardware
 * quiesce mapping.
 *
 * platform_irq_quiesce() is the only production consumer: it stops the
 * channel an unclaimed interrupt arrived on.  Channel 2 is the resident
 * timer driver's tick; channel 0's start/stop programming is
 * acceptance-only and lives under tests/target/ (tests/target/include/
 * ctc_test.h, tests/target/support/ctc_test.c); no production image
 * declares those helpers.  The ports are the *physical* channels
 * (hardware.h: channel 1 answers 42h, channel 2 answers 41h), so a
 * quiesce writes the channel the interrupt vectors for, not the address
 * that looks like its number.
 */

#include "hardware.h"

#include "kernel/irq/irq_private.h"

static __sfr __at KOS_CTC0_PORT kos_ctc0;
static __sfr __at KOS_CTC2_PORT kos_ctc2;

void platform_irq_quiesce(irq_source_t source)
{
	if (source == IRQ_CTC0) {
		kos_ctc0 = KOS_CTC_RESET;
	} else if (source == IRQ_CTC2) {
		kos_ctc2 = KOS_CTC_RESET;
	}
}
