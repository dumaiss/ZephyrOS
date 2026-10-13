/* KonaOS SIO0/B driver boot segment.
 *
 * Runs once through the mode-10 gate with Q = 6 (architecture 5.2): the
 * whole resident bank is mapped, the fixed core is not, and the driver
 * bootstrap interface is unavailable.  Nothing here executes after boot.
 *
 * The boot segment's whole job is the documented 115200 8N1 programming
 * sequence: the channel reset happens exactly once, and no run-time path
 * ever reprograms the channel (the sequence helpers live in the hot
 * segment, sio.c + sio_io.asm, which this mapping sees).
 */

#include "hardware.h"

#include "sio.h"

extern void sio0b_write_ctrl(uint8_t value);

void sio0b_hw_init(void)
{
	/* Same documented sequence as the early console; idempotent. */
	sio0b_write_ctrl(KOS_SIO_WR0_CHANNEL_RESET);

	sio0b_write_ctrl(KOS_SIO_WR_SELECT_WR4);
	sio0b_write_ctrl(KOS_SIO_WR4_X16_1STOP_NOPARITY);

	sio0b_write_ctrl(KOS_SIO_WR_SELECT_WR3);
	sio0b_write_ctrl(KOS_SIO_WR3_RX8_NO_AUTO_ENABLES);

	sio0b_write_ctrl(KOS_SIO_WR_SELECT_WR5);
	sio0b_write_ctrl(KOS_SIO_WR5_TX8_DTR_RTS_ASSERT);

	sio0b_write_ctrl(KOS_SIO_WR_SELECT_WR1);
	sio0b_write_ctrl(KOS_SIO_WR1_INT_DISABLED);
}

void sio_boot_entry(void)
{
	sio0b_hw_init();
}
