/* KonaOS libkos: the serial port held for direct I/O.
 *
 * `/dev/serial/0` (abi/kos/protocol.h, class KOS_CLASS_SERIAL).
 * serial_hold binds it exclusively and asks KSIO_OP_INFO once for the
 * data and control ports, which serial_io.asm then reads and writes
 * directly; serial_release unbinds, and the driver restores the channel.
 * A port of 0 marks "not held": the board's memory latch is port 0, so
 * the byte routines refuse it.
 *
 * Return convention matches kos/console.h: a non-negative result, or the
 * negated native error code.
 */

#include "kos.h"

#include "kos/serial.h"
#include "kos/protocol.h"

uint8_t serial_data_port;
uint8_t serial_ctrl_port;
static int serial_handle;

int serial_hold(void)
{
	struct kos_ipc_request req;
	unsigned char type = 0u;
	int handle;
	int result;

	if (serial_handle != 0) {
		return -KOS_EBUSY;
	}
	handle = bind("/dev/serial/0", KOS_BIND_EXCLUSIVE, &type);
	if (handle < 0) {
		return handle;
	}
	result = -KOS_EINVAL;
	if (type == KOS_TYPE_SERIAL) {
		req.frame.op = KSIO_OP_INFO;
		req.frame.flags = 0u;
		req.frame.desc = KOS_IPC_DESC_NONE;
		req.frame.arg0 = 0u;
		req.frame.arg1 = 0u;
		req.buffer = (void *)0;
		req.length = 0u;
		req.direction = KOS_IPC_DESC_IN;
		result = call((unsigned char)handle, &req);
		if (result >= 0) {
			result = -KOS_ENOTSUP;
			if ((req.frame.payload[KSIO_INFO_FLAGS] &
			     KSIO_CAP_DIRECT_IO) != 0u) {
				serial_data_port =
				    req.frame.payload[KSIO_INFO_DATA];
				serial_ctrl_port =
				    req.frame.payload[KSIO_INFO_CTRL];
				serial_handle = handle;
				return 0;
			}
		}
	}
	(void)unbind((unsigned char)handle);
	return result;
}

int serial_release(void)
{
	int handle = serial_handle;

	if (handle == 0) {
		return -KOS_EINVAL;
	}
	serial_handle = 0;
	serial_data_port = 0u;
	serial_ctrl_port = 0u;
	return unbind((unsigned char)handle);
}
