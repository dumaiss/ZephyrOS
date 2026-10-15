/* KonaOS TIMER driver boot segment.
 *
 * Runs once through the mode-10 gate with Q = 6 (architecture 5.2): the
 * whole resident bank is mapped, the fixed core is not, and the driver
 * bootstrap interface is unavailable.  Nothing here executes after boot.
 *
 * The boot segment's job is the channel-2 programming, from the
 * reset first: the CTC receives no hardware reset, so a warm reboot runs
 * this against whatever state the previous run left.  The sequence is the
 * documented one - reset, IM2 vector base, then the control word that
 * enables the channel, then the time constant that starts it:
 *
 *   10 MHz system clock / 256 prescaler / 250 counts = 156.25 Hz.
 *
 * The control word's mode bit matters: timer mode (D6 clear, the
 * composed KOS_CTC2_CONTROL_TIMER) counts that system clock, while
 * counter mode (D6 set) counts channel 2's CLK/TRG input - 7.3728 MHz on
 * the board - which with the same constant interrupts at ~29.5 kHz and
 * livelocks the CPU in the handler.
 *
 * The channel is *physical channel 2*, which the board's crossed select
 * wiring answers at port 41h (KOS_CTC2_PORT; 42h is channel 1): one
 * channel off, the tick would vector through entry 02h and the executive
 * would never run this driver's handler.
 *
 * Channel 3 (the event timer, port 43h) is only reset here: it stays
 * stopped until the first event period loads it, whatever state a warm
 * reboot or the power-on left it in.
 *
 * The vector base is written through physical channel 0's port (the
 * CTC's vector register is shared; D0 = 0 selects the vector write),
 * which leaves channel 0's mode untouched for the SIO0/A baud clock.
 * Channel 2's interrupt then vectors through entry 04h of the IM2 page.
 */

#include "hardware.h"

/* The vector register (shared, reached through channel 0's port) and
 * the timer's own channel, physical channel 2 at port 41h. */
static __sfr __at KOS_CTC0_PORT timer_ctc0_port;
static __sfr __at KOS_CTC2_PORT timer_ctc2_port;
static __sfr __at KOS_CTC3_PORT timer_ctc3_port;

void timer_boot_entry(void)
{
	timer_ctc3_port = KOS_CTC_RESET;
	timer_ctc2_port = KOS_CTC_RESET;
	timer_ctc0_port = KOS_CTC0_VECTOR_BASE;
	timer_ctc2_port = KOS_CTC2_CONTROL_TIMER;
	timer_ctc2_port = KOS_CTC2_TIME_CONSTANT;
}
