/* ZephyrOS SIO0/B driver implementation (drivers/sio/).
 *
 * Wraps the same physical channel as the early console (data 22h,
 * control 23h, 115200 8N1) as a resident-driver operation set with
 * bounded polling operations.  It is polling-only: no interrupts are
 * used and the SIO interrupt mode stays disabled (WR1 = 00h), which also
 * keeps it independent of the M2 executive.
 *
 * The early console and panic paths keep their direct common-resident
 * access to the hardware and do not depend on this driver.
 *
 * This source is compiled into the `sio` driver image (hot segment); the
 * driver-private state below lives in the driver's DRIVER_IO data and
 * is relocated with the image.  The driver entry points live in
 * sio_proto.c (hot) and sio_boot.c (boot).
 */

#include "zos/errno.h"

#include "hardware.h"

#include "sio.h"

extern uint8_t sio0b_status(void);
extern void sio0b_write_ctrl(uint8_t value);
extern uint8_t sio0b_read_data(void);
extern void sio0b_write_data(uint8_t value);

struct sio0b_state sio0b_state;

static uint8_t sio0b_wait_tx_ready(void)
{
	uint16_t timeout = ZOS_SIO_TX_TIMEOUT;

	while (timeout != 0) {
		if ((sio0b_status() & ZOS_SIO_TX_READY) ==
		    ZOS_SIO_TX_READY) {
			return 1;
		}
		--timeout;
	}
	return 0;
}

int sio0b_putc(void *state, char ch)
{
	(void)state;
	if (!sio0b_wait_tx_ready()) {
		++sio0b_state.tx_timeouts;
		return (-ZOS_ETIMEDOUT);
	}
	sio0b_write_data((uint8_t)ch);
	return 0;
}

int sio0b_write(void *state, const char *buf, uint16_t len)
{
	uint16_t n;

	for (n = 0; n < len; ++n) {
		if (sio0b_putc(state, buf[n]) != 0) {
			return n == 0 ? (-ZOS_ETIMEDOUT) : (int)n;
		}
	}
	return (int)len;
}

int sio0b_poll(void *state)
{
	(void)state;
	return (sio0b_status() & ZOS_SIO_RR0_RX_AVAILABLE) != 0 ? 1 : 0;
}

int sio0b_getc(void *state)
{
	(void)state;
	if ((sio0b_status() & ZOS_SIO_RR0_RX_AVAILABLE) == 0) {
		return (-ZOS_EAGAIN);
	}
	++sio0b_state.rx_reads;
	return (int)sio0b_read_data();
}
