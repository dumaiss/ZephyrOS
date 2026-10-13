/* KonaOS V9958 resident driver glue.
 *
 * The video protocol of abi/kos/protocol.h (class KOS_CLASS_VIDEO,
 * instance 0, version KOS_PROTOCOL_VERSION) in front of the driver in
 * v9958.c:
 *
 *   KVIDEO_OP_SEND   the seven native operations; the streaming
 *                    operations copy the caller's payload into this
 *                    driver's DRIVER_IO staging block in bounded
 *                    chunks and stream it;
 *   KVIDEO_OP_WRITE  terminal bytes, fed through the renderer's private
 *                    stack wrapper;
 *   KVIDEO_OP_PUTC   one terminal byte.
 *
 * The display reset (KVIDEO_RESET, ESC c, RIS) re-runs the whole
 * bring-up chain, including the font upload: this is exactly why the
 * chain is hot, and why the font upload reads the bulk-tier font through
 * the nucleus bulk copy - the mode-11 window cannot show 0000h-5FFFh of
 * bank 6.
 *
 * The init entry (mode-11 window, bootstrap interface visible) performs
 * the run-time probe: one 64-byte font chunk through bulk_read, proving
 * the extent the loader registered with this image is readable before
 * any run-time reset can need it.
 */

#include "kos/errno.h"
#include "driver.h"
#include "kos/protocol.h"

#include "v9958_driver.h"

/* Staging block in the DRIVER_IO area (8000h-BFFFh): the fast copy
 * range the streaming operations and the terminal write copy into. */
#define V9958_DRIVER_BATCH V9958_FONT_CHUNK

static const struct driver_api *v9958_driver_api;
static uint8_t v9958_batch[V9958_DRIVER_BATCH];
static uint16_t v9958_batch_address;

static int16_t v9958_driver_stream(struct kos_ipc_frame *frame)
{
	uint8_t operation = frame->arg0;
	uint16_t offset = frame->arg0;
	uint16_t length = frame->arg1;
	uint16_t done = 0u;

	(void)offset;
	/* The streaming operations' protocol rule, the same one
	 * v9958_send() applies: a zero-length transfer is refused. */
	if (length == 0u) {
		return (int16_t)-KOS_EINVAL;
	}
	while (done < length) {
		uint16_t chunk = (uint16_t)(length - done);

		if (chunk > V9958_DRIVER_BATCH) {
			chunk = V9958_DRIVER_BATCH;
		}
		if (v9958_driver_api->copy_in(frame->desc, done,
					       v9958_batch_address,
					       chunk) < 0) {
			return done != 0u ? (int16_t)done
					  : (int16_t)-KOS_EINVAL;
		}
		if (v9958_send(&v9958_state, operation, v9958_batch,
			       chunk) != 0) {
			return (int16_t)-KOS_EINVAL;
		}
		done = (uint16_t)(done + chunk);
	}
	return (int16_t)done;
}

static int16_t v9958_driver_write(struct kos_ipc_frame *frame)
{
	uint16_t offset = frame->arg0;
	uint16_t length = frame->arg1;
	uint16_t done = 0u;

	while (done < length) {
		uint16_t chunk = (uint16_t)(length - done);
		int result;

		if (chunk > V9958_DRIVER_BATCH) {
			chunk = V9958_DRIVER_BATCH;
		}
		if (v9958_driver_api->copy_in(frame->desc,
					       (uint16_t)(offset + done),
					       v9958_batch_address,
					       chunk) < 0) {
			return done != 0u ? (int16_t)done
					  : (int16_t)-KOS_EINVAL;
		}
		result = v9958_char_write(&v9958_state,
					  (const char *)v9958_batch, chunk);
		if (result < 0) {
			return done != 0u ? (int16_t)done : (int16_t)result;
		}
		done = (uint16_t)(done + chunk);
	}
	return (int16_t)done;
}

int16_t v9958_driver_dispatch(struct kos_ipc_frame *frame)
{
	switch (frame->op) {
	case KVIDEO_OP_SEND:
		if (frame->arg0 == KVIDEO_PRESENT ||
		    frame->arg0 == KVIDEO_RESET) {
			return (int16_t)v9958_send(&v9958_state, frame->arg0,
						   (const void *)0, 0u);
		}
		return v9958_driver_stream(frame);
	case KVIDEO_OP_WRITE:
		return v9958_driver_write(frame);
	case KVIDEO_OP_PUTC:
		return (int16_t)v9958_char_putc(&v9958_state,
						(char)frame->arg0);
	default:
		return (int16_t)-KOS_EINVAL;
	}
}

uint8_t v9958_driver_init(const struct driver_boot *boot)
{
	uint8_t probe[V9958_FONT_CHUNK];
	const struct driver_api *api = boot->api;

	v9958_driver_api = api;
	v9958_set_api(api);
	v9958_batch_address = (uint16_t)v9958_batch;
	/* From here every font read is the run-time nucleus bulk copy; the
	 * boot segment's direct source has done its job. */
	v9958_set_font_base(0u);
	(void)api->bulk_read(0u, (uint16_t)probe, V9958_FONT_CHUNK);
	return 0u;
}
