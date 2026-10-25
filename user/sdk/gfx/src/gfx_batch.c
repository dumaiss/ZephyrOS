/* KonaOS libgfx: command batching (docs/gfx-sdk.md section 9).
 *
 * Between gfx_begin and the matching gfx_end, drawing calls append
 * 16-byte records to the process-local queue instead of starting VDP
 * commands; gfx_end flushes when the outermost batch closes, and a
 * full queue flushes itself.  gfx_begin/gfx_end nest (the depth is
 * counted, at most GFX_BATCH_DEPTH_MAX, as libtui's transaction
 * depth).  gfx_flush executes the queued records at once, wherever the
 * depth stands.  A record submitted outside a batch executes
 * immediately; the queue is walked in order, never reordered.
 *
 * No VIDEO IPC: the backend programs and starts each command through
 * the ports, one at a time.
 */

#include "kos.h"

#include "kos/gfx.h"
#include "gfx_internal.h"

void gfx_begin(void)
{
	if (gfx_state.video_handle == 0) {
		return;
	}
	if (gfx_state.batch_depth < GFX_BATCH_DEPTH_MAX) {
		++gfx_state.batch_depth;
	}
}

int gfx_end(void)
{
	if (gfx_state.video_handle == 0) {
		return -KOS_EINVAL;
	}
	if (gfx_state.batch_depth == 0u) {
		return -KOS_EINVAL;
	}
	--gfx_state.batch_depth;
	if (gfx_state.batch_depth != 0u) {
		return 0;
	}
	return gfx_flush();
}

int gfx_flush(void)
{
	if (gfx_state.video_handle == 0) {
		return -KOS_EINVAL;
	}
	if (gfx_state.batch_count != 0u) {
		v9958_run_batch(&gfx_state, gfx_state.batch_queue,
				gfx_state.batch_count);
		gfx_state.batch_count = 0u;
	}
	return 0;
}

int gfx_wait(void)
{
	if (gfx_state.video_handle == 0) {
		return -KOS_EINVAL;
	}
	gfx_v9958_io_wait_command();
	return 0;
}

void gfx_timing_mark(void)
{
	if (gfx_state.video_handle == 0) {
		return;
	}
	gfx_v9958_io_reg(15u, GFX_V9958_REG_R16);
}

void gfx_batch_submit(const struct gfx_cmd *cmd)
{
	if (gfx_state.batch_depth == 0u) {
		/* Outside a batch a drawing call executes at once. */
		v9958_run_batch(&gfx_state, cmd, 1u);
		return;
	}
	if (gfx_state.batch_count == GFX_BATCH_ENTRIES) {
		/* The queue is full: flush it, then take the record. */
		v9958_run_batch(&gfx_state, gfx_state.batch_queue,
				gfx_state.batch_count);
		gfx_state.batch_count = 0u;
	}
	gfx_state.batch_queue[gfx_state.batch_count] = *cmd;
	++gfx_state.batch_count;
}
