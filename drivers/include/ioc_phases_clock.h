/* Read-path phase instrument's clock (drivers, `KOS_IOC_PHASES=1` only).
 *
 * The half of drivers/include/ioc_phases.h a *boot segment* needs and
 * nothing more: the instrument's own CTC channel and the one-time program
 * that starts it.  Keeping it apart is what lets drivers/ioc/ioc_boot.c
 * start the clock without pulling the stamping code - and its reference
 * byte - into a segment that never stamps anything.
 *
 * Channel 1 is the one physical channel no shipped image programs
 * (hardware.h: it answers at port 42h, the board's crossed select
 * wiring), free-running in timer mode on the internal 10 MHz / 256 clock
 * with interrupts off: no request may reach the IM2 page, and the counter
 * runs either way.  Its constant is 0, meaning 256 counts - one cycle is
 * 6.5536 ms, coprime enough with the timer's 250 that a pair of readings
 * from the two channels resolves to one count difference inside an
 * 0.8192 s window (see ioc_phases.h).
 */

#ifndef KONAOS_DRIVERS_IOC_PHASES_CLOCK_H
#define KONAOS_DRIVERS_IOC_PHASES_CLOCK_H

#include "hardware.h"

#if KOS_IOC_PHASES

/* Physical channel 1 (port 42h) and the timer driver's channel 2 (port
 * 41h): the pair a stamp reads. */
static __sfr __at KOS_CTC1_PORT ioc_phases_ctc1;
static __sfr __at KOS_CTC2_PORT ioc_phases_ctc2;

/* Channel 1's one-shot programming: reset, timer mode on the internal
 * 10 MHz / 256 clock with interrupts off, then the constant. */
#define IOC_PHASES_CTC1_CONTROL (KOS_CTC_CTRL_PRESCALER_256 | \
				 KOS_CTC_CTRL_TIME_CONSTANT | \
				 KOS_CTC_CTRL_CONTROL_WORD)
#define IOC_PHASES_CTC1_CONSTANT 0x00u

/* The boot segment's one-time channel-1 program.  A warm reboot stops
 * every channel (platform_ctc_quiesce_all), so this runs on every boot,
 * not just the first. */
static void ioc_phases_ctc1_init(void)
{
	ioc_phases_ctc1 = KOS_CTC_RESET;
	ioc_phases_ctc1 = IOC_PHASES_CTC1_CONTROL;
	ioc_phases_ctc1 = IOC_PHASES_CTC1_CONSTANT;
}

#endif /* KOS_IOC_PHASES */

#endif /* KONAOS_DRIVERS_IOC_PHASES_CLOCK_H */
