/* KonaOS libgfx: the lifecycle and the process-local graphics state.
 *
 * gfx_open is the only VIDEO IPC the library makes: one exclusive bind
 * of `/dev/video`, the object-type check, one KVIDEO_OP_DIRECT_INFO
 * and the backend selection.  Every later call works the ports the
 * record reported, directly.  The release (gfx_close, or the
 * process's exit, which releases the handle the kernel still holds)
 * puts the console back.
 */

#include "kos.h"

#include "kos/protocol.h"
#include "kos/gfx.h"
#include "gfx_internal.h"

struct gfx_state gfx_state;

/* One descriptor-less call; the reply payload is the record. */
static int gfx_call_direct_info(struct kvideo_direct_info *info)
{
	struct kos_ipc_request req;
	int result;

	req.frame.op = KVIDEO_OP_DIRECT_INFO;
	req.frame.flags = 0u;
	req.frame.desc = KOS_IPC_DESC_NONE;
	req.frame.arg0 = 0u;
	req.frame.arg1 = 0u;
	req.buffer = (void *)0;
	req.length = 0u;
	req.direction = KOS_IPC_DESC_IN;
	result = call((unsigned char)gfx_state.video_handle, &req);
	if (result < 0) {
		return result;
	}
	memcpy(info, req.frame.payload, sizeof(struct kvideo_direct_info));
	return 0;
}

int gfx_open(void)
{
	struct kvideo_direct_info info;
	unsigned char type = 0u;
	int handle;
	int result;

	if (gfx_state.video_handle != 0) {
		return -KOS_EBUSY;
	}
	handle = bind("/dev/video", KOS_BIND_EXCLUSIVE, &type);
	if (handle < 0) {
		return handle;
	}
	gfx_state.video_handle = handle;
	if (type != KOS_TYPE_VIDEO) {
		gfx_close();
		return -KOS_EINVAL;
	}
	result = gfx_call_direct_info(&info);
	if (result < 0) {
		gfx_close();
		return result;
	}
	if (info.version != KVIDEO_DIRECT_INFO_VERSION ||
	    info.backend != KVIDEO_BACKEND_V9958) {
		gfx_close();
		return -KOS_ENOTSUP;
	}
	gfx_state.backend = info.backend;
	gfx_state.caps = info.flags;
	gfx_state.info = info;
	/* The drawing assembly reaches the ports through the mirrored
	 * bytes from here on. */
	v9958_load_ports(&gfx_state.info);
	/* The RAM font starts from the built-in (gfx_font.c). */
	gfx_font_install();
	gfx_draw_open = 1u;
	return 0;
}

void gfx_close(void)
{
	if (gfx_state.video_handle == 0) {
		return;
	}
	/* The release hands the screen back to the console: finish any
	 * command still drawing, so the engine is idle for the console's
	 * bring-up. */
	if (gfx_state.mode_set != 0u) {
		gfx_v9958_io_wait_command();
	}
	(void)unbind((unsigned char)gfx_state.video_handle);
	gfx_state.video_handle = 0;
	gfx_draw_open = 0u;
	gfx_draw_ready = 0u;
	gfx_draw_shape = 0u;
	/* The queue dies with the handle: whatever was pending never
	 * runs, and the next open starts an empty batch. */
	gfx_batch_depth = 0u;
	gfx_batch_count = 0u;
}

int gfx_get_info(struct gfx_info *info)
{
	if (info == (struct gfx_info *)0) {
		return -KOS_EINVAL;
	}
	if (gfx_state.video_handle == 0 || gfx_state.mode_set == 0u) {
		return -KOS_EINVAL;
	}
	info->mode = (enum gfx_mode)gfx_state.mode;
	info->width = gfx_state.width;
	info->height = gfx_state.height;
	info->bpp = gfx_state.bpp;
	info->page = gfx_state.shown_page;
	return 0;
}
