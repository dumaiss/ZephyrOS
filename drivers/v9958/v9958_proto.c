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
 *   KVIDEO_OP_PUTC   one terminal byte;
 *   KVIDEO_OP_DIRECT_INFO the direct-graphics record (abi/kos/kvideo.h),
 *                    served only while the object is held exclusively;
 *   KOS_OP_SUBSCRIBE the vertical-blank event, for the program holding
 *                    the VDP exclusively (below).
 *
 * The display reset (KVIDEO_RESET, ESC c, RIS) re-runs the whole
 * bring-up chain: this is exactly why the chain is hot.
 *
 * The init entry (mode-11 window, bootstrap interface visible) builds the
 * renderer's expansion pages in the mapped segment, inside a bulk-map
 * span (v9958_expand_init).
 */

#include "kos/errno.h"
#include "driver.h"
#include "kos/protocol.h"
#include "kos/console_render.h"
#include "kos/kvideo.h"

#include "hardware.h"

#include "v9958_driver.h"

/* Staging block in the DRIVER_IO area (8000h-BFFFh): the fast copy
 * range the streaming operations and the terminal write copy into. */
#define V9958_DRIVER_BATCH 64u

static const struct driver_api *v9958_driver_api;
static uint8_t v9958_batch[V9958_DRIVER_BATCH];
static uint8_t *v9958_batch_ptr;
static uint16_t v9958_batch_address;

static int16_t v9958_render_copy(const struct kos_ipc_frame *frame,
				 uint16_t at, uint16_t length)
{
	if (v9958_driver_api->copy_in(frame->desc,
			(uint16_t)((uint16_t)frame->arg0 + at),
			v9958_batch_address, length) < 0) {
		return (int16_t)-KOS_EINVAL;
	}
	return 0;
}

/* Validate the complete stream before the renderer touches VRAM. */
static int16_t v9958_render_validate(const struct kos_ipc_frame *frame)
{
	uint16_t length = frame->arg1;
	uint16_t at = KCONSOLE_RENDER_HEADER_SIZE;

	if (frame->desc == KOS_IPC_DESC_NONE ||
	    length < KCONSOLE_RENDER_HEADER_SIZE ||
	    length > KOS_IPC_DESC_MAX ||
	    (uint16_t)(frame->arg0 + length) < frame->arg0 ||
	    v9958_render_copy(frame, 0u, KCONSOLE_RENDER_HEADER_SIZE) < 0) {
		return (int16_t)-KOS_EINVAL;
	}
	if (v9958_batch_ptr[0] != KCONSOLE_RENDER_VERSION ||
	    v9958_batch_ptr[1] != 0u ||
	    (uint16_t)(v9958_batch_ptr[2] |
	       ((uint16_t)v9958_batch_ptr[3] << 8)) !=
		length) {
		return (int16_t)-KOS_EINVAL;
	}
	while (at < length) {
		uint8_t op;
		uint8_t record_length;

		if ((uint16_t)(length - at) < 2u ||
		    v9958_render_copy(frame, at, 2u) < 0) {
			return (int16_t)-KOS_EINVAL;
		}
		op = v9958_batch_ptr[0];
		record_length = v9958_batch_ptr[1];
		if (record_length < 2u ||
		    (uint16_t)record_length > (uint16_t)(length - at)) {
			return (int16_t)-KOS_EINVAL;
		}
		if (op == KCONSOLE_RENDER_TEXT ||
		    op == KCONSOLE_RENDER_INVERT) {
			uint8_t count;

			if (record_length < KCONSOLE_RENDER_SPAN_BASE ||
			    v9958_render_copy(frame, at,
					       KCONSOLE_RENDER_SPAN_BASE) < 0) {
				return (int16_t)-KOS_EINVAL;
			}
			count = v9958_batch_ptr[5];
			if (count == 0u ||
			    record_length !=
				(uint8_t)(KCONSOLE_RENDER_SPAN_BASE + count) ||
			    v9958_batch_ptr[2] >= V9958_COLUMNS ||
			    v9958_batch_ptr[3] >= V9958_ROWS ||
			    (v9958_batch_ptr[4] & (uint8_t)~KCONSOLE_ATTR_REVERSE) != 0u ||
			    count > (uint8_t)(V9958_COLUMNS -
					      v9958_batch_ptr[2])) {
				return (int16_t)-KOS_EINVAL;
			}
		} else if (op == KCONSOLE_RENDER_STATE) {
			if (record_length != KCONSOLE_RENDER_STATE_SIZE ||
			    (uint16_t)(at + record_length) != length ||
			    v9958_render_copy(frame, at,
					       KCONSOLE_RENDER_STATE_SIZE) < 0 ||
			    v9958_batch_ptr[2] >= V9958_COLUMNS ||
			    v9958_batch_ptr[3] >= V9958_ROWS ||
			    (v9958_batch_ptr[4] & (uint8_t)~KCONSOLE_ATTR_REVERSE) != 0u ||
			    v9958_batch_ptr[5] > 1u) {
				return (int16_t)-KOS_EINVAL;
			}
		} else if (op == KCONSOLE_RENDER_SCROLL) {
			if (record_length != KCONSOLE_RENDER_SCROLL_SIZE ||
			    v9958_render_copy(frame, at,
					       KCONSOLE_RENDER_SCROLL_SIZE) < 0 ||
			    v9958_batch_ptr[2] >= V9958_ROWS ||
			    v9958_batch_ptr[3] == 0u ||
			    v9958_batch_ptr[3] >
				(uint8_t)(V9958_ROWS - v9958_batch_ptr[2]) ||
			    v9958_batch_ptr[4] > KCONSOLE_SCROLL_DOWN ||
			    (v9958_batch_ptr[5] &
				(uint8_t)~KCONSOLE_ATTR_REVERSE) != 0u) {
				return (int16_t)-KOS_EINVAL;
			}
		} else {
			return (int16_t)-KOS_EINVAL;
		}
		at = (uint16_t)(at + record_length);
	}
	return length == KCONSOLE_RENDER_HEADER_SIZE ?
		(int16_t)-KOS_EINVAL : 0;
}

/* Second pass.  The assembly entry calls this on the renderer's private
 * stack and brackets the whole walk with sprites disabled (skipped while
 * a program holds the VDP exclusively). */
int v9958_driver_render_worker(struct kos_ipc_frame *frame)
{
	uint16_t length = frame->arg1;
	uint16_t at = KCONSOLE_RENDER_HEADER_SIZE;

	if (v9958_overtaken(&v9958_state, length) != 0u) {
		return (int)length;
	}
	v9958_state.render_active = 1u;
	while (at < length) {
		uint8_t op;
		uint8_t record_length;
		uint8_t x;
		uint8_t y;
		uint8_t attr;
		uint8_t count;

		if (v9958_render_copy(frame, at, KCONSOLE_RENDER_SPAN_BASE) < 0) {
			return -KOS_EINVAL;
		}
		op = v9958_batch_ptr[0];
		record_length = v9958_batch_ptr[1];
		x = v9958_batch_ptr[2];
		y = v9958_batch_ptr[3];
		attr = v9958_batch_ptr[4];
		count = v9958_batch_ptr[5];
		if (op == KCONSOLE_RENDER_TEXT) {
			uint16_t data_at =
				(uint16_t)(at + KCONSOLE_RENDER_SPAN_BASE);
			uint8_t left = count;

			v9958_out_flush();
			v9958_state.text_col = x;
			v9958_state.text_row = y;
			v9958_state.current_attr = attr;
			while (left != 0u) {
				uint8_t chunk = left;

				if (chunk > V9958_DRIVER_BATCH) {
					chunk = V9958_DRIVER_BATCH;
				}
				if (v9958_render_copy(frame, data_at, chunk) < 0) {
					return -KOS_EINVAL;
				}
				v9958_render_text_data(v9958_batch_ptr, chunk);
				data_at = (uint16_t)(data_at + chunk);
				left = (uint8_t)(left - chunk);
			}
		} else if (op == KCONSOLE_RENDER_INVERT) {
			v9958_render_invert(x, y, count);
		} else if (op == KCONSOLE_RENDER_SCROLL) {
			v9958_render_scroll(x, y, attr, count);
		} else {
			v9958_out_flush();
			v9958_state.text_col = x;
			v9958_state.text_row = y;
			v9958_state.current_attr = attr;
			v9958_state.cursor_visible = count;
		}
		at = (uint16_t)(at + record_length);
	}
	return (int)length;
}

/* --- the vertical-blank event ------------------------------------------
 *
 * The program holding the VDP exclusively may subscribe (SYS_NOTIFY on its
 * handle) to the V9958's frame interrupt.  The interrupt reaches the CPU
 * on /INT (the configuration latch's D0 = 0, which this driver keeps) as
 * the source IRQ_VDP_FF, since the VDP supplies no IM2 vector.  The hold
 * claims the source (an unclaimed one would not be acknowledged, and a
 * holder may enable the interrupt without subscribing); the subscription
 * selects status register 0, and the handler reads it, which clears the
 * frame flag and releases /INT, and posts the event, delivered when the
 * interrupt returns into the program.  The frame interrupt itself (IE0 in
 * R#1) is the program's to enable, as on a ColecoVision: it writes R#1
 * anyway.  The release runs the display init (IE0 off, status register 2
 * selected) before it drops the claim.
 *
 * Reading status resets the command port's two-byte latch: a program's own
 * register writes must not be split by the interrupt (write them from the
 * event handler, or masked), as under a ColecoVision's VDP NMI. */
extern void v9958_io_select_status0(void);
extern void v9958_io_vblank_ack(void);
extern uint8_t v9958_status0;

static uint8_t v9958_vblank_sub;     /* the subscription id, 0 = none */

static void v9958_vblank_irq(void *context)
{
	(void)context;
	v9958_io_vblank_ack();
	if (v9958_vblank_sub != 0u) {
		v9958_driver_api->event_post(v9958_vblank_sub);
	}
}

static int16_t v9958_vblank_subscribe(uint8_t sub)
{
	if (sub != 0u) {
		if (v9958_state.exclusive == 0u || v9958_vblank_sub != 0u) {
			return (int16_t)-KOS_EBUSY;
		}
		if (v9958_status0 == 0u) {
			v9958_io_select_status0();
		}
	}
	v9958_vblank_sub = sub; /* 0: the hold keeps the acknowledge */
	return 0;
}

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

/* Served only to the exclusive holder: the record is the takeover's
 * contract, not a general hardware query.  The reply payload is the
 * struct kvideo_direct_info (the layout is pinned in kos/kvideo.h): the
 * V9958 backend, the four capabilities, 128 KiB, the five ports
 * A0h-A4h and the console's latch value - 0, the value the bring-up
 * always establishes, so it needs no write.  The reserved bytes and the
 * frame's trailing bytes are cleared in one tail run. */
static int16_t v9958_direct_info(struct kos_ipc_frame *frame)
{
	uint8_t *dst = frame->payload;
	uint8_t left = KOS_IPC_PAYLOAD_SIZE - 11u;

	if (v9958_state.exclusive == 0u) {
		return (int16_t)-KOS_EBUSY;
	}
	*dst++ = KVIDEO_DIRECT_INFO_VERSION;
	*dst++ = KVIDEO_BACKEND_V9958;
	*dst++ = (uint8_t)(KVIDEO_DIRECT_IO | KVIDEO_HW_WAIT |
			   KVIDEO_PALETTE | KVIDEO_YJK);
	*dst++ = 0u; /* the flags' high byte */
	*dst++ = 128u;
	*dst++ = 0u; /* vram_kib's high byte */
	*dst++ = KOS_V9958_DATA_PORT;
	*dst++ = KOS_V9958_CMD_PORT;
	*dst++ = KOS_V9958_PALETTE_PORT;
	*dst++ = KOS_V9958_INDIRECT_PORT;
	*dst++ = KOS_V9958_CONFIG_PORT;
	do {
		*dst++ = 0u;
	} while (--left != 0u);
	return 0;
}

int16_t v9958_driver_dispatch(struct kos_ipc_frame *frame)
{
	switch (frame->op) {
	case KOS_OP_ACQUIRE:
		/* One object, one exclusive holder: the console's own
		 * handles are ordinary binds, so the request only ever
		 * meets another takeover. */
		if (v9958_state.exclusive != 0u) {
			return (int16_t)-KOS_EBUSY;
		}
		if (v9958_driver_api->irq_claim(KOS_IRQ_VDP_FF,
						(uint16_t)v9958_vblank_irq,
						0u) != 0) {
			return (int16_t)-KOS_EBUSY;
		}
		v9958_state.exclusive = 1u;
		v9958_state.dropped = 0u;
		return 0;
	case KOS_OP_RELEASE:
		if (frame->arg1 != 0u) {
			/* The takeover ended: the program may have left the
			 * VDP in any state, so the driver re-runs the whole
			 * display init and the console draws again.  The
			 * init turns the frame interrupt off and selects
			 * status register 2 before the claim is dropped. */
			int16_t result;

			v9958_state.exclusive = 0u;
			v9958_vblank_sub = 0u;
			v9958_status0 = 0u;
			result = (int16_t)v9958_display_reset();
			(void)v9958_driver_api->irq_release(KOS_IRQ_VDP_FF);
			return result;
		}
		return 0;
	case KOS_OP_SUBSCRIBE:
		return v9958_vblank_subscribe(frame->arg0);
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
	case KVIDEO_OP_RESET:
		return (int16_t)v9958_display_reset();
	case KVIDEO_OP_INFO: {
		/* The terminal's grid and attributes (KCONSOLE_OP_INFO); the
		 * reserved bytes are zero. */
		uint8_t *info = frame->payload;
		uint8_t left = KCONSOLE_INFO_SIZE - 4u;

		*info++ = V9958_COLUMNS;
		*info++ = V9958_ROWS;
		*info++ = KCONSOLE_ATTR_REVERSE;
		*info++ = KCONSOLE_INFO_RENDER;
		do {
			*info++ = 0u;
		} while (--left != 0u);
		return 0;
	}
	case KVIDEO_OP_RENDER:
		if (v9958_render_validate(frame) < 0) {
			return (int16_t)-KOS_EINVAL;
		}
		return (int16_t)v9958_render_transaction(frame);
	case KVIDEO_OP_DIRECT_INFO:
		return v9958_direct_info(frame);
	default:
		return (int16_t)-KOS_EINVAL;
	}
}

uint8_t v9958_driver_init(const struct driver_boot *boot)
{
	const struct driver_api *api = boot->api;

	v9958_driver_api = api;
	v9958_set_api(api);
	v9958_batch_address = (uint16_t)v9958_batch;
	v9958_batch_ptr = v9958_batch;
	v9958_expand_init();
	return 0u;
}
