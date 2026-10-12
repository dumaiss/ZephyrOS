/* ZephyrOS M0 early serial console (Z80 SIO channel B).
 *
 * This is the permanent low-level diagnostic path, not the future console
 * subsystem.  It is compiled into the _COMMON code segment so the whole
 * path (code and string literals) is resident in the permanently mapped
 * common region (E000h-FEFFh) and remains usable during mode/bank
 * transitions and kernel failures.
 *
 * Programming sequence (115200 8N1, polled, interrupts disabled):
 *   WR0 = 18h  channel reset
 *   WR4 = 44h  x16 clock, 1 stop bit, no parity
 *   WR3 = C1h  8-bit RX, RX enabled, Auto Enables off
 *   WR5 = EAh  TX enabled, 8-bit TX, DTR asserted, RTS asserted
 *   WR1 = 00h  interrupts disabled
 *
 * Transmit polls RR0 until TX-empty (04h) is set, with a finite timeout so
 * a missing terminal cannot hang the kernel forever; the console is never
 * gated by the FT230X modem-handshake crossover (CTS/RTS).
 * No printf(), no input API, no drivers, no interrupts.
 */

#include "kernel.h"

static __sfr __at ZOS_SIO0B_CTRL zos_sio_b_ctrl;
static __sfr __at ZOS_SIO0B_DATA zos_sio_b_data;

void early_console_init(void)
{
	zos_sio_b_ctrl = ZOS_SIO_WR0_CHANNEL_RESET;

	zos_sio_b_ctrl = ZOS_SIO_WR_SELECT_WR4;
	zos_sio_b_ctrl = ZOS_SIO_WR4_X16_1STOP_NOPARITY;

	zos_sio_b_ctrl = ZOS_SIO_WR_SELECT_WR3;
	zos_sio_b_ctrl = ZOS_SIO_WR3_RX8_NO_AUTO_ENABLES;

	zos_sio_b_ctrl = ZOS_SIO_WR_SELECT_WR5;
	zos_sio_b_ctrl = ZOS_SIO_WR5_TX8_DTR_RTS_ASSERT;

	zos_sio_b_ctrl = ZOS_SIO_WR_SELECT_WR1;
	zos_sio_b_ctrl = ZOS_SIO_WR1_INT_DISABLED;
}

void early_console_putc(char ch)
{
	uint16_t timeout = ZOS_SIO_TX_TIMEOUT;

	while (timeout--) {
		uint8_t status = zos_sio_b_ctrl;

		if ((status & ZOS_SIO_TX_READY) == ZOS_SIO_TX_READY) {
			zos_sio_b_data = ch;
			return;
		}
	}

	/* Timeout: drop the character rather than hang the kernel. */
}

void early_console_puts(const char *s)
{
	while (*s) {
		early_console_putc(*s++);
	}
}
