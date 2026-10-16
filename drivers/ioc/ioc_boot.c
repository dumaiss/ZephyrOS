/* KonaOS IOC driver boot segment.
 *
 * Runs once through the mode-10 gate with Q = 6 (architecture 5.2): the
 * whole resident bank is mapped, the fixed core is not, and the driver
 * bootstrap interface is unavailable.  Nothing here executes after boot.
 *
 * The boot segment programs both lanes once (SIO1/B external sync with
 * the receiver disabled, SIO1/A armed without Hunt).  The bounded
 * LINK_SYNC handshake is not here: it needs no bootstrap interface, but
 * it is also the run-time whole-link recovery path, so it lives in the
 * hot segment with the rest of the transport.
 *
 * In a measurement build (KOS_IOC_PHASES) the segment also programs CTC
 * channel 1, the read-path instrument's free-running reference channel: a
 * boot segment is where a channel survives the warm reboot that stops
 * every channel (platform_ctc_quiesce_all) and re-programs what the
 * running image needs.
 */

#include "hardware.h"

#include "ioc_driver.h"

/* The read-path instrument's second clock channel: only this build has
 * anything to program here, and the boot segment wants the clock alone
 * (drivers/include/ioc_phases_clock.h). */
#include "ioc_phases_clock.h"

extern uint8_t ioc_io_status_sample(void);
extern void ioc_io_ctrl_write(uint8_t value);
extern void ioc_io_bulk_init(void);
extern uint8_t ioc_io_irq_save_disable(void);
extern void ioc_io_irq_restore(uint8_t token);

void ioc_hw_init(void)
{
	/* The command lane's one and only channel reset: after this,
	 * ordinary transactions never touch synchronisation state. */
	ioc_io_ctrl_write(KOS_IOC_WR0_RESET_CHANNEL);

	/* WR1: no interrupts, no WAIT/DMA. */
	ioc_io_ctrl_write(KOS_SIO_WR_SELECT_WR1);
	ioc_io_ctrl_write(KOS_IOC_WR1_INT_WAIT_OFF);

	/* WR4: External Sync, x1 clock. */
	ioc_io_ctrl_write(KOS_SIO_WR_SELECT_WR4);
	ioc_io_ctrl_write(KOS_IOC_WR4_EXTSYNC_X1);

	/* WR7: FFh underrun fill (never the A5 packet marker). */
	ioc_io_ctrl_write(KOS_IOC_WR_SELECT_WR7);
	ioc_io_ctrl_write(KOS_IOC_WR7_UNDERRUN_FILL);

	/* WR3: receiver disabled; no character boundary exists yet. */
	ioc_io_ctrl_write(KOS_SIO_WR_SELECT_WR3);
	ioc_io_ctrl_write(KOS_IOC_WR3_RX_OFF);

	/* WR5: 8-bit TX enabled, RTS inactive. */
	ioc_io_ctrl_write(KOS_SIO_WR_SELECT_WR5);
	ioc_io_ctrl_write(KOS_IOC_WR5_TX_RTS_OFF);

	/* The bulk lane is programmed once here too, in module-owned
	 * assembly: channel reset, External Sync x1, AAh fill, receiver
	 * left enabled without Hunt, transmitter idle.  The first
	 * verified bulk read enters Hunt exactly once; after that the
	 * boundary is never disturbed. */
	ioc_io_bulk_init();

#if KOS_IOC_PHASES
	/* The read-path instrument's free-running reference channel.  It
	 * belongs in a boot segment: the channel is the instrument's own
	 * (no shipped image programs channel 1) and a warm reboot stops
	 * every channel, and then re-runs this programming. */
	ioc_phases_ctc1_init();
#endif
}

void ioc_boot_entry(void)
{
	ioc_hw_init();
}
