/* KonaOS libkos: the game-controller client over the JOY driver.
 *
 * `/dev/input/joy/<pad>` (abi/kos/protocol.h, class KOS_CLASS_JOY).  Both
 * operations are descriptor-less.  joy_open asks KJOY_OP_INFO once for
 * the pad's port and keeps it, with the pad, in a table indexed by the
 * handle; joy_read (joy_read.asm) reads that port directly, which is the
 * every-frame path, and joy_read_call goes through the driver.  The
 * decoders are joy_decode.c.
 *
 * Return convention matches kos/console.h: a non-negative result, or the
 * negated native error code.
 */

#include "kos.h"

#include "kos/joy.h"
#include "kos/protocol.h"

/* A handle number is below JOY_HANDLE_SLOTS (the handle table holds 12
 * entries); joy_read.asm indexes with the number's low four bits.  A
 * port of 0 marks a handle joy_open did not return. */
#define JOY_HANDLE_SLOTS 16u

/* The name of pad 0; the last character is the pad's digit. */
#define JOY_PATH "/dev/input/joy/0"

uint8_t joy_port[JOY_HANDLE_SLOTS];
static uint8_t joy_pad[JOY_HANDLE_SLOTS];

static int joy_call(int joy, uint8_t op, uint8_t pad,
		    struct kos_ipc_request *req)
{
	req->frame.op = op;
	req->frame.flags = 0u;
	req->frame.desc = KOS_IPC_DESC_NONE;
	req->frame.arg0 = pad;
	req->frame.arg1 = 0u;
	req->buffer = (void *)0;
	req->length = 0u;
	req->direction = KOS_IPC_DESC_IN;
	return call((unsigned char)joy, req);
}

int joy_open(uint8_t pad)
{
	char path[sizeof(JOY_PATH)];
	struct kos_ipc_request req;
	unsigned char type = 0u;
	int joy;
	int result;

	if (pad > 9u) {
		return -KOS_ENOENT;
	}
	memcpy(path, JOY_PATH, sizeof(path));
	path[sizeof(path) - 2u] = (char)('0' + pad);
	joy = bind(path, 0u, &type);
	if (joy < 0) {
		return joy;
	}
	result = -KOS_EINVAL;
	if (type == KOS_TYPE_JOY && (unsigned int)joy < JOY_HANDLE_SLOTS) {
		result = joy_call(joy, KJOY_OP_INFO, 0u, &req);
		if (result >= 0) {
			result = -KOS_EINVAL;
			if (pad < req.frame.payload[KJOY_INFO_COUNT]) {
				joy_port[joy] =
				    req.frame.payload[KJOY_INFO_PORTS + pad];
				joy_pad[joy] = pad;
				return joy;
			}
		}
	}
	(void)unbind((unsigned char)joy);
	return result;
}

int joy_close(int joy)
{
	joy_port[joy & (int)(JOY_HANDLE_SLOTS - 1u)] = 0u;
	return unbind((unsigned char)joy);
}

int joy_read_call(int joy, uint8_t *raw)
{
	struct kos_ipc_request req;
	int result;

	if (raw == (uint8_t *)0) {
		return -KOS_EINVAL;
	}
	result = joy_call(joy, KJOY_OP_READ,
			  joy_pad[joy & (int)(JOY_HANDLE_SLOTS - 1u)], &req);
	if (result < 0) {
		return result;
	}
	*raw = req.frame.payload[0];
	return 0;
}
