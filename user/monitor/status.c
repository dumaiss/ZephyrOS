/* ZephyrOS user runtime: common status/error output.
 *
 * See status.h for the vocabulary.
 */

#include "status.h"
#include "zos.h"
#include "hexfmt.h"

void zrt_status_ok(void)
{
	zos_puts("OK\r\n");
}

void zrt_status_err_arg(void)
{
	zos_puts("ERR: ARG\r\n");
}

void zrt_status_err_command(void)
{
	zos_puts("ERR: COMMAND\r\n");
}

void zrt_status_err_code(int status)
{
	char digits[3];
	unsigned int code;

	if (status < 0) {
		code = (unsigned int)(0u - (unsigned int)status);
	} else {
		code = (unsigned int)status;
	}
	zos_puts("ERR: ");
	zrt_fmt_hex8(digits, code & 0xFFu);
	zos_puts(digits);
	zos_puts("\r\n");
}
