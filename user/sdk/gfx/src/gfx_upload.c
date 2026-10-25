/* KonaOS libgfx: the streaming pixel upload.
 *
 * gfx_upload_begin names a rectangle; gfx_upload_write streams payload
 * bytes through the data port with the position kept across calls, and
 * gfx_upload_end closes the transfer.  The byte stream follows the
 * current mode's packing, so the transfer takes exactly
 * bytes(width, height) and a chunk that would run past the end is
 * refused whole.  One VRAM address is set per row (one for the whole
 * transfer when the rectangle spans the full width, whose rows are
 * contiguous).  A pending drawing batch is flushed first, so the
 * stream lands after every queued command; the upload cannot be
 * nested, and the off-screen page (y 256-511) is an ordinary target.
 *
 * No VIDEO IPC: every write is direct I/O through the backend's
 * assembly port layer.
 */

#include "kos.h"

#include "kos/gfx.h"
#include "gfx_internal.h"

/* The rectangle's payload bytes per row and per transfer. */
static uint16_t gfx_upload_row_bytes_of(uint16_t width)
{
	return (gfx_state.mode == (uint8_t)GFX_MODE_512x212_16) ?
		(uint16_t)(width / 2u) : width;
}

/* The backend's VRAM address for the current row start. */
static void gfx_upload_address(void)
{
	uint16_t offset = (uint16_t)((uint16_t)gfx_state.upload_row * 256u +
				     gfx_state.upload_col_byte);
	uint8_t r14 = (uint8_t)(((offset >> 14) & 0x03u) |
				(gfx_state.upload_page << 2));

	gfx_v9958_io_set_vram(r14, offset);
}

int gfx_upload_begin(uint16_t x, uint16_t y, uint16_t width,
		     uint16_t height)
{
	uint16_t full_width = (gfx_state.mode == (uint8_t)GFX_MODE_512x212_16)
		? GFX_WIDTH_G6 : GFX_WIDTH_G7;

	if (gfx_state.video_handle == 0 || gfx_state.mode_set == 0u ||
	    gfx_state.upload_active != 0u) {
		return -KOS_EINVAL;
	}
	if (width == 0u || height == 0u || y >= 512u ||
	    (uint16_t)(y + height) > 512u || x >= full_width ||
	    (uint16_t)(x + width) > full_width) {
		return -KOS_EINVAL;
	}
	if (gfx_state.mode == (uint8_t)GFX_MODE_512x212_16 &&
	    ((x | width) & 1u) != 0u) {
		return -KOS_EINVAL;
	}
	/* Four YJK pixels share one J/K pair: x and width keep the runs. */
	if (gfx_state.mode == (uint8_t)GFX_MODE_256x212_YJK &&
	    ((x | width) & 3u) != 0u) {
		return -KOS_EINVAL;
	}
	/* A pending drawing batch flushes first, so the stream's bytes
	 * land after every command the application queued. */
	if (gfx_flush() < 0) {
		return -KOS_EINVAL;
	}
	/* And the stream waits for the engine, so its bytes land after
	 * the last command the flush issued (the batch returns with it
	 * still drawing). */
	gfx_v9958_io_wait_command();
	gfx_state.upload_active = 1u;
	gfx_state.upload_full_width = (x == 0u && width == full_width) ? 1u
		: 0u;
	gfx_state.upload_addressed = 0u;
	gfx_state.upload_page = (uint8_t)(y >> 8);
	gfx_state.upload_row = (uint8_t)(y & 0xFFu);
	gfx_state.upload_col_byte = (gfx_state.mode ==
				     (uint8_t)GFX_MODE_512x212_16) ?
		(uint16_t)(x / 2u) : x;
	gfx_state.upload_row_bytes = gfx_upload_row_bytes_of(width);
	gfx_state.upload_row_left = 0u;
	gfx_state.upload_remaining =
		(uint16_t)(gfx_state.upload_row_bytes * height);
	return 0;
}

int gfx_upload_write(const void *data, uint16_t length)
{
	const uint8_t *src = (const uint8_t *)data;

	if (gfx_state.upload_active == 0u) {
		return -KOS_EINVAL;
	}
	if (data == (const void *)0 && length != 0u) {
		return -KOS_EINVAL;
	}
	if (length > gfx_state.upload_remaining) {
		/* The chunk is refused whole: the position is unchanged,
		 * so the caller can send a fitting chunk. */
		return -KOS_EINVAL;
	}
	if (gfx_state.upload_full_width != 0u) {
		if (gfx_state.upload_addressed == 0u) {
			gfx_upload_address();
			gfx_state.upload_addressed = 1u;
		}
		gfx_v9958_io_stream_vram(src, length);
		gfx_state.upload_remaining =
			(uint16_t)(gfx_state.upload_remaining - length);
		return (int)length;
	}
	while (length != 0u) {
		uint16_t chunk = length;

		if (gfx_state.upload_row_left == 0u) {
			gfx_upload_address();
			gfx_state.upload_row_left =
				gfx_state.upload_row_bytes;
		}
		if (chunk > gfx_state.upload_row_left) {
			chunk = gfx_state.upload_row_left;
		}
		gfx_v9958_io_stream_vram(src, chunk);
		gfx_state.upload_row_left =
			(uint16_t)(gfx_state.upload_row_left - chunk);
		gfx_state.upload_remaining =
			(uint16_t)(gfx_state.upload_remaining - chunk);
		src += chunk;
		length = (uint16_t)(length - chunk);
		if (gfx_state.upload_row_left == 0u &&
		    gfx_state.upload_remaining != 0u) {
			++gfx_state.upload_row;
			if (gfx_state.upload_row == 0u) {
				++gfx_state.upload_page;
			}
		}
	}
	return (int)(src - (const uint8_t *)data);
}

int gfx_upload_end(void)
{
	if (gfx_state.upload_active == 0u) {
		return -KOS_EINVAL;
	}
	gfx_state.upload_active = 0u;
	if (gfx_state.upload_remaining != 0u) {
		/* Bytes are missing: the upload is cancelled, never left
		 * half-open. */
		return -KOS_EINVAL;
	}
	return 0;
}
