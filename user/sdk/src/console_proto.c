/* ZephyrOS libzos: console client over the console driver (package 41).
 *
 * The console is a bank-6 driver now (abi/zos/protocol.h, class
 * ZOS_CLASS_CONSOLE) and the three SYS_CONSOLE_* syscalls are gone, so the
 * SDK's console functions are ordinary driver clients:
 *
 *   console_write()/console_read()
 *                           one descriptor-bearing WRITE/READ call, with
 *                           the caller's buffer named by the request's
 *                           descriptor (the nucleus captures it from the
 *                           syscall gate's latch, architecture 36.8);
 *   console_poll()          the descriptor-less POLL operation;
 *   putchar()               the descriptor-less one-byte PUTC operation:
 *                           no descriptor capture, no copy, one call.
 *
 * The console is the `/dev/console` object and the two standard handles
 * every process starts with (architecture 36.13): ZOS_HANDLE_STDOUT for
 * output, ZOS_HANDLE_STDIN for input, bound at process start and inherited
 * across SYS_EXEC.  There is no lookup and no cache; a program whose build
 * published no console gets -ZOS_EINVAL from the call, because the standard
 * handles are then absent.
 *
 * Return convention matches zos/console.h: a non-negative result, or the
 * negated native error code.
 */

#include "zos.h"

#include "zos/protocol.h"

#define console_handle() ZOS_HANDLE_STDOUT
#define console_input_handle() ZOS_HANDLE_STDIN

int console_write(const char *buf, unsigned int len)
{
	struct zos_ipc_request req;
	int handle;

	if (len == 0) {
		return 0;
	}
	if (buf == (const char *)0) {
		return -ZOS_EINVAL;
	}
	handle = console_handle();
	req.frame.op = ZCONSOLE_OP_WRITE;
	req.frame.flags = 0;
	req.frame.arg0 = 0u;
	req.frame.arg1 = len;
	req.buffer = (void *)buf;
	req.length = len;
	req.direction = ZOS_IPC_DESC_IN;
	return call((unsigned char)handle, &req);
}

int console_read(char *buf, unsigned int len)
{
	struct zos_ipc_request req;
	int handle;

	if (len == 0) {
		return 0;
	}
	if (buf == (char *)0) {
		return -ZOS_EINVAL;
	}
	handle = console_input_handle();
	req.frame.op = ZCONSOLE_OP_READ;
	req.frame.flags = 0;
	req.frame.arg0 = 0u;
	req.frame.arg1 = len;
	req.buffer = (void *)buf;
	req.length = len;
	req.direction = ZOS_IPC_DESC_OUT;
	return call((unsigned char)handle, &req);
}

int console_poll(void)
{
	struct zos_ipc_request req;
	int handle;

	handle = console_input_handle();
	req.frame.op = ZCONSOLE_OP_POLL;
	req.frame.flags = 0;
	req.frame.arg0 = 0u;
	req.frame.arg1 = 0u;
	req.buffer = (void *)0;
	req.length = 0u;
	req.direction = ZOS_IPC_DESC_IN;
	return call((unsigned char)handle, &req);
}

/* One console byte without a descriptor: the form the protocol reserves for
 * callers with nothing to name.  `putchar` returns the byte, or the
 * negative error. */
int putchar(int ch)
{
	struct zos_ipc_request req;
	unsigned char handle = console_handle();
	int result;

	req.frame.op = ZCONSOLE_OP_PUTC;
	req.frame.flags = 0;
	req.frame.arg0 = (unsigned char)ch;
	req.frame.arg1 = 0u;
	req.buffer = (void *)0;
	req.length = 0u;
	req.direction = ZOS_IPC_DESC_IN;
	result = call((unsigned char)handle, &req);
	return (result < 0) ? result : (int)(unsigned char)ch;
}
