/* KonaOS SIO0/B resident driver (work package 41).
 *
 * The M8 `sio` driver as a bank-6 driver image: the SIO0/B channel
 * (115200 8N1, the same physical channel as the early console) behind the
 * serial protocol of abi/kos/protocol.h (class KOS_CLASS_SERIAL,
 * instance 0, version KOS_PROTOCOL_VERSION).
 *
 * Segments (architecture 36.6):
 *   hot  this file: the protocol dispatch and the bounded operation
 *        bodies; the driver's raw port I/O helpers (sio_io.asm) and
 *        private state;
 *   boot sio_boot.c: the one 115200 8N1 programming sequence, which runs
 *        once through the mode-10 gate and is never needed again (the
 *        channel reset leaves the receiver enabled and the transmitter
 *        idle, so ordinary operations never reprogram the channel).
 *
 * The driver owns no IRQ: the SIO interrupt mode stays disabled
 * (WR1 = 00h), exactly as the early console leaves it.
 *
 * Data movement: WRITE copies the caller's buffer into this driver's
 * DRIVER_IO staging block in bounded chunks; READ drains the receiver
 * into the same block.  Both are descriptor copies (36.8) - the driver
 * never names a caller address.
 */

#include "kos/errno.h"
#include "driver.h"
#include "kos/protocol.h"

#include "sio.h"

/* Bounded staging block in the driver I/O area (8000h-BFFFh): the one
 * range both the mode-11 window and the mode-10 aperture show, so a
 * copy_in/copy_out over it takes the fast paths. */
#define SIO_DRIVER_BATCH 64u

static const struct driver_api *sio_api;
static uint8_t sio_batch[SIO_DRIVER_BATCH];
static uint16_t sio_batch_address;

static int16_t sio_write_buffer(struct kos_ipc_frame *frame)
{
	uint16_t offset = frame->arg0;
	uint16_t length = frame->arg1;
	uint16_t done = 0u;

	while (done < length) {
		uint16_t chunk = (uint16_t)(length - done);
		int16_t result;

		if (chunk > SIO_DRIVER_BATCH) {
			chunk = SIO_DRIVER_BATCH;
		}
		result = sio_api->copy_in(frame->desc,
					  (uint16_t)(offset + done),
					  sio_batch_address, chunk);
		if (result < 0) {
			return done != 0u ? (int16_t)done : result;
		}
		if (sio0b_write(&sio0b_state, (const char *)sio_batch,
				chunk) != (int)chunk) {
			return done != 0u ? (int16_t)done
					  : (int16_t)-KOS_ETIMEDOUT;
		}
		done = (uint16_t)(done + chunk);
	}
	return (int16_t)done;
}

static int16_t sio_read_buffer(struct kos_ipc_frame *frame)
{
	uint16_t offset = frame->arg0;
	uint16_t length = frame->arg1;
	uint16_t done = 0u;

	if (length == 0u) {
		return 0;
	}
	while (done < length) {
		uint16_t chunk = 0u;

		while (chunk < SIO_DRIVER_BATCH && done < length) {
			int ch = sio0b_getc(&sio0b_state);

			if (ch < 0) {
				break;
			}
			sio_batch[chunk] = (uint8_t)ch;
			++chunk;
			++done;
		}
		if (chunk == 0u) {
			break;
		}
		if (sio_api->copy_out(frame->desc, (uint16_t)(offset + done -
							      chunk),
				      sio_batch_address, chunk) < 0) {
			return (int16_t)-KOS_EINVAL;
		}
	}
	return (int16_t)done;
}

int16_t sio_driver_dispatch(struct kos_ipc_frame *frame)
{
	switch (frame->op) {
	case KSIO_OP_PUTC:
		return (int16_t)sio0b_putc(&sio0b_state, (char)frame->arg0);
	case KSIO_OP_WRITE:
		return sio_write_buffer(frame);
	case KSIO_OP_POLL:
		return (int16_t)sio0b_poll(&sio0b_state);
	case KSIO_OP_GETC:
		return (int16_t)sio0b_getc(&sio0b_state);
	case KSIO_OP_READ:
		return sio_read_buffer(frame);
	default:
		return (int16_t)-KOS_EINVAL;
	}
}

uint8_t sio_driver_init(const struct driver_boot *boot)
{
	sio_api = boot->api;
	sio_batch_address = (uint16_t)sio_batch;
	return 0u;
}
