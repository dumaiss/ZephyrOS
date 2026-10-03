/* ZephyrOS M7 native monitor: memory commands (D/DB/M/MB).
 *
 * Every access crosses SYS_DIAG_MEM_READ / SYS_DIAG_MEM_WRITE, so the
 * kernel re-validates the bank (0-6), the read range 0000h-DFFFh, the
 * write range 0100h-DFFFh (native page zero protected), and restores
 * the exact active latch.  The monitor rejects malformed, trailing,
 * out-of-range, and masked-out bank arguments itself with ERR: ARG
 * before the syscall, so operator mistakes never reach the kernel.
 */

#include "monitor.h"
#include "console_io.h"
#include "hexfmt.h"
#include "hexdump.h"
#include "status.h"

/* Reads allow 0000h-DFFFh; one past the window is the range limit. */
#define MON_MEM_READ_LIMIT 0xE000u

static void mon_mem_error(int result)
{
	if (result < 0) {
		zrt_status_err_code(result);
	}
}

/* Dump `len` bytes from `bank` at `addr`, 16 bytes per row. */
static void mon_dump(unsigned char bank, unsigned int addr,
		     unsigned int len)
{
	unsigned char row[16];
	char text[ZRT_HEXDUMP_ROW_MAX];

	while (len != 0) {
		unsigned int count = (len > 16u) ? 16u : len;
		unsigned int i;

		for (i = 0; i < count; ++i) {
			struct zex_diag_mem_read req;
			int value;

			req.bank = bank;
			req.reserved = 0;
			req.address = (unsigned int)(addr + i);
			value = zex_diag_mem_read(&req);
			if (value < 0) {
				mon_mem_error(value);
				return;
			}
			row[i] = (unsigned char)value;
		}
		zrt_hexdump_row(text, addr, row, count);
		zrt_puts(text);
		zrt_puts("\r\n");
		addr = (unsigned int)(addr + count);
		len -= count;
	}
}

void mon_cmd_mem(char *cursor, unsigned char with_bank,
		 unsigned char modify)
{
	unsigned char bank = mon_bank;
	unsigned int addr;
	unsigned int length;
	unsigned int value;

	if (with_bank && !mon_scan_bank(&cursor, &bank)) {
		zrt_status_err_arg();
		return;
	}
	if (!zrt_parse_hex(&cursor, &addr)) {
		zrt_status_err_arg();
		return;
	}
	if (modify) {
		struct zex_diag_mem_write req;
		int result;

		if (!zrt_parse_hex8(&cursor, &value) ||
		    !zrt_line_end(&cursor)) {
			zrt_status_err_arg();
			return;
		}
		if (addr < ZEX_DIAG_MEM_WRITE_MIN ||
		    addr > ZEX_DIAG_MEM_WRITE_MAX) {
			zrt_status_err_arg();
			return;
		}
		req.bank = bank;
		req.value = (unsigned char)value;
		req.address = addr;
		result = zex_diag_mem_write(&req);
		if (result < 0) {
			mon_mem_error(result);
			return;
		}
		zrt_status_ok();
		return;
	}

	if (!zrt_parse_hex(&cursor, &length) || !zrt_line_end(&cursor)) {
		zrt_status_err_arg();
		return;
	}
	if (addr >= MON_MEM_READ_LIMIT ||
	    length > (unsigned int)(MON_MEM_READ_LIMIT - addr)) {
		zrt_status_err_arg();
		return;
	}
	mon_dump(bank, addr, length);
}
