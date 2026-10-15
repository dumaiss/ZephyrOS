/* Production CTC owner: the logical-source to Zephyr-80 hardware
 * quiesce mapping.
 *
 * platform_irq_quiesce() stops the channel an unclaimed interrupt arrived
 * on.  Channel 2 is the resident timer driver's tick and channel 3 its
 * event timer - an application that subscribed to device events, or a
 * driver image that failed to load, must not leave either running; the
 * timer driver stops channel 3 itself when its handle is released, and
 * this is the unconditional backstop the panic path uses.  Channel 0's
 * start/stop programming is acceptance-only and lives under tests/target/
 * (tests/target/include/ctc_test.h, tests/target/support/ctc_test.c); no
 * production image declares those helpers.  The ports are the *physical*
 * channels (hardware.h: channel 1 answers 42h, channel 2 answers 41h), so
 * a quiesce writes the channel the interrupt vectors for, not the address
 * that looks like its number.
 *
 * platform_ctc_quiesce_all() is the warm reboot's: the CTC is not reset
 * by the reset vector, so every channel this kernel may have programmed
 * keeps counting and requesting into the next boot unless it is stopped
 * here.  All four physical channels are stopped by their own ports,
 * including channel 1, which no KonaOS image programs - a quiesce is a
 * two-byte control write either way, and a channel left armed by another
 * agent must not interrupt the boot.
 */

#include "hardware.h"

#include "kernel/irq/irq_private.h"

static __sfr __at KOS_CTC0_PORT kos_ctc0;
static __sfr __at KOS_CTC1_PORT kos_ctc1;
static __sfr __at KOS_CTC2_PORT kos_ctc2;
static __sfr __at KOS_CTC3_PORT kos_ctc3;

void platform_irq_quiesce(irq_source_t source)
{
	if (source == IRQ_CTC0) {
		kos_ctc0 = KOS_CTC_RESET;
	} else if (source == IRQ_CTC2) {
		kos_ctc2 = KOS_CTC_RESET;
	} else if (source == IRQ_CTC3) {
		kos_ctc3 = KOS_CTC_RESET;
	}
}

void platform_ctc_quiesce_all(void)
{
	kos_ctc0 = KOS_CTC_RESET;
	kos_ctc1 = KOS_CTC_RESET;
	kos_ctc2 = KOS_CTC_RESET;
	kos_ctc3 = KOS_CTC_RESET;
}
