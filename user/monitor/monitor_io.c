/* KonaOS native monitor: I/O port commands (I/O).
 *
 * Ports 00h-0Fh alias the banking latch on Zephyr-80 and are rejected by
 * both this program and SYS_DIAG_IO_IN/OUT; the monitor reports them as
 * ERR: ARG before issuing the syscall.  All other 8-bit ports go through
 * the diagnostic boundary, which never changes the selected bank.
 */

#include "monitor.h"
#include "kos.h"
#include "diag.h" /* the SDK diag wrappers */
#include "hexfmt.h"
#include "status.h"

void mon_cmd_io_in(char *cursor)
{
	struct kos_diag_io_in_args req;
	unsigned int port;
	int value;

	if (!zrt_parse_hex8(&cursor, &port) || !zrt_line_end(&cursor)) {
		zrt_status_err_arg();
		return;
	}
	if (port < KOS_DIAG_IO_PORT_MIN) {
		zrt_status_err_arg(); /* latch alias: refused, not executed */
		return;
	}
	req.port = (unsigned char)port;
	value = diag_io_in(&req);
	if (value < 0) {
		zrt_status_err_code(value);
		return;
	}
	console_puts("IN=");
	mon_put_hex8((unsigned int)value);
	console_puts("\r\n");
}

void mon_cmd_io_out(char *cursor)
{
	struct kos_diag_io_out_args req;
	unsigned int port;
	unsigned int value;
	int result;

	if (!zrt_parse_hex8(&cursor, &port) ||
	    !zrt_parse_hex8(&cursor, &value) ||
	    !zrt_line_end(&cursor)) {
		zrt_status_err_arg();
		return;
	}
	if (port < KOS_DIAG_IO_PORT_MIN) {
		zrt_status_err_arg(); /* latch alias: refused, not executed */
		return;
	}
	req.port = (unsigned char)port;
	req.value = (unsigned char)value;
	result = diag_io_out(&req);
	if (result < 0) {
		zrt_status_err_code(result);
		return;
	}
	zrt_status_ok();
}
