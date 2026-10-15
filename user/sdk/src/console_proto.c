/* KonaOS libkos: console client over the console driver.
 *
 * The console is a bank-6 driver (abi/kos/protocol.h, class
 * KOS_CLASS_CONSOLE) and the three SYS_CONSOLE_* syscalls are gone, so the
 * SDK's console functions are ordinary driver clients:
 *
 *   console_write()/console_read()
 *                           one descriptor-bearing WRITE/READ call, with
 *                           the caller's buffer named by the request's
 *                           descriptor (the nucleus captures it from the
 *                           syscall gate's latch, architecture 4.9);
 *   console_poll()          the descriptor-less POLL operation;
 *   putchar()               one byte into the output buffer;
 *   console_flush()         write the output buffer out now.
 *
 * Output is line-buffered: one 128-byte buffer per program.  putchar,
 * console_puts and printf append to it; a console_write longer than the
 * free space flushes it first and then writes directly.  The buffer is
 * written out on '\n', when it fills, before every console read, before
 * `run` and at exit (the exit path calls the flush; console_flush is the
 * explicit call).  An error in the flushed write is returned by the call
 * that flushed, except putchar, which keeps returning its byte.  A
 * program that faults loses whatever is still buffered.
 *
 * The console is the `/dev/console` object and the two standard handles
 * every process starts with (architecture 4.10): KOS_HANDLE_STDOUT for
 * output, KOS_HANDLE_STDIN for input, bound fresh at process start - a
 * child started by SYS_EXEC gets its own.  There is no lookup and no
 * cache; a program whose build published no console gets -KOS_EINVAL
 * from the call, because the standard handles are then absent.
 *
 * Return convention matches kos/console.h: a non-negative result, or the
 * negated native error code.
 */

#include "kos.h"

#include "kos/protocol.h"

#define console_handle() KOS_HANDLE_STDOUT
#define console_input_handle() KOS_HANDLE_STDIN

/* One output buffer per program: a screen line's worth, and a descriptor
 * well inside the 4,096 bytes the protocol allows. */
#define KOS_CONSOLE_BUF_SIZE 128u

static char console_buf[KOS_CONSOLE_BUF_SIZE];
static unsigned int console_len;

/* The stream layer's exit hook: `fopen` sets it, so a program that never
 * opens a stream links none of the stream code while exit still flushes
 * whatever a program did open. */
void (*kos_stream_flush)(void);

/* One descriptor-bearing write: the path every flush and every write
 * longer than the buffer takes. */
static int console_send(const char *buf, unsigned int len)
{
	struct kos_ipc_request req;
	int handle;

	handle = console_handle();
	req.frame.op = KCONSOLE_OP_WRITE;
	req.frame.flags = 0;
	req.frame.arg0 = 0u;
	req.frame.arg1 = len;
	req.buffer = (void *)buf;
	req.length = len;
	req.direction = KOS_IPC_DESC_IN;
	return call((unsigned char)handle, &req);
}

int console_flush(void)
{
	int result = 0;

	if (console_len != 0u) {
		unsigned int len = console_len;
		int sent;

		/* The bytes are consumed whether the write succeeds or
		 * not: the buffer never replays them. */
		console_len = 0u;
		sent = console_send(console_buf, len);
		result = sent < 0 ? sent : 0;
	}
	return result;
}

/* Append one byte, flushing the buffer when the byte fills it and at a
 * line's end.  Returns 0 or the error of a flush this byte caused. */
static int console_append(char c)
{
	int result = 0;

	console_buf[console_len] = c;
	++console_len;
	if (console_len == KOS_CONSOLE_BUF_SIZE || c == '\n') {
		result = console_flush();
	}
	return result;
}

int console_write(const char *buf, unsigned int len)
{
	unsigned int i;
	int result = 0;

	if (len == 0u) {
		return 0;
	}
	if (buf == (const char *)0) {
		return -KOS_EINVAL;
	}
	if (len > KOS_CONSOLE_BUF_SIZE - console_len) {
		result = console_flush();
		if (result < 0) {
			return result;
		}
		return console_send(buf, len);
	}
	for (i = 0u; i < len; ++i) {
		int flushed = console_append(buf[i]);

		if (flushed < 0 && result == 0) {
			result = flushed;
		}
	}
	return result == 0 ? (int)len : result;
}

int console_read(char *buf, unsigned int len)
{
	struct kos_ipc_request req;
	int handle;

	if (len == 0) {
		return 0;
	}
	if (buf == (char *)0) {
		return -KOS_EINVAL;
	}
	(void)console_flush();
	handle = console_input_handle();
	req.frame.op = KCONSOLE_OP_READ;
	req.frame.flags = 0;
	req.frame.arg0 = 0u;
	req.frame.arg1 = len;
	req.buffer = (void *)buf;
	req.length = len;
	req.direction = KOS_IPC_DESC_OUT;
	return call((unsigned char)handle, &req);
}

int console_poll(void)
{
	struct kos_ipc_request req;
	int handle;

	(void)console_flush();
	handle = console_input_handle();
	req.frame.op = KCONSOLE_OP_POLL;
	req.frame.flags = 0;
	req.frame.arg0 = 0u;
	req.frame.arg1 = 0u;
	req.buffer = (void *)0;
	req.length = 0u;
	req.direction = KOS_IPC_DESC_IN;
	return call((unsigned char)handle, &req);
}

/* One console byte without a descriptor, through the output buffer:
 * `putchar` returns the byte, as ISO requires.  A failed flush this byte
 * caused is not reported here - the bytes are already gone - so a
 * caller that must see the error flushes explicitly
 * (kos/console.h). */
int putchar(int ch)
{
	(void)console_append((char)(unsigned char)ch);
	return (int)(unsigned char)ch;
}

/* Exit-path flush: the console buffer and, when the stream layer is
 * linked, every open stream.  `exit` and the startup code's return call
 * it before the SYS_EXIT that ends the process. */
void kos_exit_flush(void)
{
	(void)console_flush();
	if (kos_stream_flush != (void (*)(void))0) {
		kos_stream_flush();
	}
}
