/* KonaOS libkos: the mouse client over the HID driver's MOUSE endpoint,
 * and the pointer client over the video driver's KVIDEO_OP_POINTER.
 *
 * `/dev/input/mouse` (abi/kos/protocol.h, class KOS_CLASS_MOUSE) answers
 * KMOUSE_OP_READ with the controller's 9-byte report in the reply
 * payload; the call is descriptor-less, so the gate captures nothing.
 * The report's layout is struct mouse_report's (bytes, then three
 * little-endian words), so a read is one copy.
 *
 * The position is this client's policy: the driver forwards deltas and
 * never clamps.  The pointer call is descriptor-less too; the video
 * driver checks the range.
 */

#include "kos.h"

#include "kos/mouse.h"
#include "kos/protocol.h"

_Static_assert(sizeof(struct mouse_report) == KMOUSE_REPORT_LEN,
	       "struct mouse_report is the controller's report layout");

int mouse_open(void)
{
	unsigned char type = 0u;
	int handle = bind("/dev/input/mouse", 0u, &type);

	if (handle < 0) {
		return handle;
	}
	if (type != KOS_TYPE_MOUSE) {
		(void)unbind((unsigned char)handle);
		return -KOS_EINVAL;
	}
	return handle;
}

int mouse_read(int handle, struct mouse_report *r)
{
	struct kos_ipc_request req;
	int result;

	if (r == (struct mouse_report *)0) {
		return -KOS_EINVAL;
	}
	req.frame.op = KMOUSE_OP_READ;
	req.frame.flags = 0u;
	req.frame.desc = KOS_IPC_DESC_NONE;
	req.frame.arg0 = 0u;
	req.frame.arg1 = 0u;
	req.buffer = (void *)0;
	req.length = 0u;
	req.direction = KOS_IPC_DESC_IN;
	result = call((unsigned char)handle, &req);
	if (result < 0) {
		return result;
	}
	memcpy(r, req.frame.payload, KMOUSE_REPORT_LEN);
	return 0;
}

void mouse_init(struct mouse *m, int handle, int width, int height)
{
	m->handle = handle;
	m->width = width;
	m->height = height;
	m->x = (int)((unsigned int)width >> 1);
	m->y = (int)((unsigned int)height >> 1);
	m->wheel = 0;
	m->buttons = 0u;
	m->pressed = 0u;
	m->present = 0u;
}

/* `at` moved by `delta`, inside 0..size-1.  Each branch stays in range:
 * at >= 0 and delta >= -32768 below, size - at > 0 above. */
static int mouse_clamp(int at, int delta, int size)
{
	if (delta < 0) {
		at += delta;
		return at < 0 ? 0 : at;
	}
	if (delta >= size - at) {
		return size - 1;
	}
	return at + delta;
}

int mouse_track(struct mouse *m)
{
	struct mouse_report r;
	int result = mouse_read(m->handle, &r);
	int x;
	int y;
	uint8_t present;
	uint8_t changed;

	if (result < 0) {
		return result;
	}
	x = m->x;
	y = m->y;
	if (r.dx != 0) {
		x = mouse_clamp(x, r.dx, m->width);
	}
	if (r.dy != 0) {
		y = mouse_clamp(y, r.dy, m->height);
	}
	present = (r.flags & MOUSE_PRESENT) != 0u ? 1u : 0u;
	changed = (uint8_t)(x != m->x || y != m->y || r.wheel != 0 ||
			    r.pressed != 0u || r.buttons != m->buttons ||
			    present != m->present);
	m->x = x;
	m->y = y;
	m->wheel += r.wheel;
	m->pressed |= r.pressed;
	m->buttons = r.buttons;
	m->present = present;
	return changed;
}

int pointer_open(void)
{
	unsigned char type = 0u;
	int handle = bind("/dev/video", 0u, &type);

	if (handle < 0) {
		return handle;
	}
	if (type != KOS_TYPE_VIDEO) {
		(void)unbind((unsigned char)handle);
		return -KOS_EINVAL;
	}
	return handle;
}

static int pointer_call(int video, unsigned int x, unsigned int y)
{
	struct kos_ipc_request req;

	req.frame.op = KVIDEO_OP_POINTER;
	req.frame.flags = 0u;
	req.frame.desc = KOS_IPC_DESC_NONE;
	req.frame.arg0 = (uint8_t)y;
	req.frame.arg1 = x;
	req.buffer = (void *)0;
	req.length = 0u;
	req.direction = KOS_IPC_DESC_IN;
	return call((unsigned char)video, &req);
}

int pointer_move(int video, int x, int y)
{
	if (x < 0 || y < 0) {
		return -KOS_EINVAL;
	}
	return pointer_call(video, (unsigned int)x, (unsigned int)y);
}

int pointer_hide(int video)
{
	return pointer_call(video, KVIDEO_POINTER_HIDE, 0u);
}
