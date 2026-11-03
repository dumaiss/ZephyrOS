/* KonaOS SIO0/B driver boot segment.
 *
 * Runs once through the mode-10 gate with Q = 6 (architecture 5.2): the
 * whole resident bank is mapped, the fixed core is not, and the driver
 * bootstrap interface is unavailable.  Nothing here executes after boot.
 *
 * The boot segment's whole job is the documented 115200 8N1 programming
 * sequence: the channel reset happens exactly once.  The setup that
 * follows it lives in the hot segment (sio_io.asm, which this mapping
 * sees), because the end of an exclusive hold rewrites it.
 */

#include "hardware.h"

#include "sio.h"

extern void sio0b_write_ctrl(uint8_t value);

void sio0b_hw_init(void)
{
	/* Same documented sequence as the early console; idempotent.  The
	 * setup after the reset is the hot segment's, which the end of an
	 * exclusive hold reruns. */
	sio0b_write_ctrl(KOS_SIO_WR0_CHANNEL_RESET);
	sio0b_setup();
}

void sio_boot_entry(void)
{
	sio0b_hw_init();
}
