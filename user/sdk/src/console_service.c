/* ZephyrOS libzos: console client over the console service (package 41).
 *
 * The console is a bank-6 service now (abi/zos/protocol.h, class
 * ZOS_CLASS_CONSOLE) and the three SYS_CONSOLE_* syscalls are gone, so the
 * SDK's console functions are ordinary service clients:
 *
 *   zex_write()/zex_read()  one descriptor-bearing WRITE/READ call, with
 *                           the caller's buffer named by the request's
 *                           descriptor (the nucleus captures it from the
 *                           syscall gate's latch, architecture 36.8);
 *   zex_poll()              the descriptor-less POLL operation;
 *   zex_putc()              the descriptor-less one-byte PUTC operation,
 *                           which is what zos_putc() uses: no descriptor
 *                           capture, no copy, one call.
 *
 * The endpoint handle is looked up once and cached (services are
 * permanent).  Return convention matches zos/console.h: a non-negative
 * result, or the negated native error code; a program with no console
 * service sees -ZOS_ENOENT from the lookup.
 */

#include "zos.h"

#include "zos/protocol.h"

static unsigned char console_endpoint;

static int console_handle(void)
{
	int handle = (int)console_endpoint;

	if (handle != 0) {
		return handle;
	}
	handle = zos_ipc_lookup(ZOS_CLASS_CONSOLE, ZOS_CLASS_INSTANCE);
	if (handle <= 0) {
		return handle;
	}
	console_endpoint = (unsigned char)handle;
	return handle;
}

int zex_write(const char *buf, unsigned int len)
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
	if (handle <= 0) {
		return handle;
	}
	req.frame.op = ZCONSOLE_OP_WRITE;
	req.frame.flags = 0;
	req.frame.arg0 = 0u;
	req.frame.arg1 = len;
	req.buffer = (void *)buf;
	req.length = len;
	req.direction = ZOS_IPC_DESC_IN;
	return zos_ipc_call((unsigned char)handle, &req);
}

int zex_read(char *buf, unsigned int len)
{
	struct zos_ipc_request req;
	int handle;

	if (len == 0) {
		return 0;
	}
	if (buf == (char *)0) {
		return -ZOS_EINVAL;
	}
	handle = console_handle();
	if (handle <= 0) {
		return handle;
	}
	req.frame.op = ZCONSOLE_OP_READ;
	req.frame.flags = 0;
	req.frame.arg0 = 0u;
	req.frame.arg1 = len;
	req.buffer = (void *)buf;
	req.length = len;
	req.direction = ZOS_IPC_DESC_OUT;
	return zos_ipc_call((unsigned char)handle, &req);
}

int zex_poll(void)
{
	struct zos_ipc_request req;
	int handle;

	handle = console_handle();
	if (handle <= 0) {
		return handle;
	}
	req.frame.op = ZCONSOLE_OP_POLL;
	req.frame.flags = 0;
	req.frame.arg0 = 0u;
	req.frame.arg1 = 0u;
	req.buffer = (void *)0;
	req.length = 0u;
	req.direction = ZOS_IPC_DESC_IN;
	return zos_ipc_call((unsigned char)handle, &req);
}

/* One console byte without a descriptor: the form the protocol reserves for
 * callers with nothing to name. */
int zex_putc(int ch)
{
	struct zos_ipc_request req;
	int handle = console_handle();

	if (handle <= 0) {
		return handle;
	}
	req.frame.op = ZCONSOLE_OP_PUTC;
	req.frame.flags = 0;
	req.frame.arg0 = (unsigned char)ch;
	req.frame.arg1 = 0u;
	req.buffer = (void *)0;
	req.length = 0u;
	req.direction = ZOS_IPC_DESC_IN;
	return zos_ipc_call((unsigned char)handle, &req);
}
