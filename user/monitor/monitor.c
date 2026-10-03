/* ZephyrOS M7 native monitor: startup, info/help commands, dispatch loop.
 *
 * See monitor.h for the command surface.  The monitor resolves its own
 * bank and the platform bank mask once at startup (SYS_DIAG_BANK_INFO),
 * prints a banner that includes a safe operator scratch address, then
 * dispatches edited 80-character command lines until Q exits through
 * SYS_EXIT.
 */

#include "monitor.h"
#include "console_io.h"
#include "hexfmt.h"
#include "status.h"

unsigned char mon_bank;
unsigned char mon_mask;
unsigned char mon_line[MON_LINE_MAX];

/* Safe one-byte operator scratch inside this program's BSS: `M`/`D` on
 * its address (printed in the banner) exercise the diagnostic memory
 * path without touching live monitor state. */
static unsigned char mon_scratch;

static const char mon_banner[] = "\r\nZephyrOS native monitor\r\n";
static const char mon_prompt[] = "Z80> ";
static const char mon_bank_label[] = "bank=";
static const char mon_pid_label[] = " pid=";
static const char mon_scratch_label[] = " scratch=";
static const char mon_line_end[] = "\r\n";
static const char mon_fail_diag[] =
	"monitor: diagnostic services unavailable\r\n";

static const char mon_help[] =
	"R              system/bank information\r\n"
	"D addr len     dump monitor bank (16-byte rows)\r\n"
	"DB bank addr len  dump bank 0-6\r\n"
	"M addr value   set byte in monitor bank\r\n"
	"MB bank addr value set byte in bank 0-6\r\n"
	"I port         input port (10-FF)\r\n"
	"O port value   output port (10-FF)\r\n"
	"Q              quit\r\n"
	"H or ?         help\r\n";

void mon_put_hex8(unsigned int value)
{
	char digits[3];

	zrt_fmt_hex8(digits, value);
	zrt_puts(digits);
}

void mon_put_hex16(unsigned int value)
{
	char digits[5];

	zrt_fmt_hex16(digits, value);
	zrt_puts(digits);
}

unsigned char mon_scan_bank(char **cursor, unsigned char *bank)
{
	unsigned int value;

	if (!zrt_parse_hex8(cursor, &value)) {
		return 0;
	}
	if (value > 7u) {
		return 0;
	}
	if ((mon_mask & (unsigned char)(1u << (unsigned char)value)) == 0) {
		return 0; /* mask bit clear: not a valid application bank */
	}
	*bank = (unsigned char)value;
	return 1;
}

static void mon_put_u8_dec(unsigned int value)
{
	char digits[4];
	unsigned char hundreds = 0;
	unsigned char tens = 0;

	/* 0-255 without division: repeated subtraction. */
	while (value >= 100u) {
		value -= 100u;
		++hundreds;
	}
	while (value >= 10u) {
		value -= 10u;
		++tens;
	}
	digits[0] = (char)('0' + hundreds);
	digits[1] = (char)('0' + tens);
	digits[2] = (char)('0' + (unsigned char)value);
	digits[3] = '\0';
	if (digits[0] != '0') {
		zrt_puts(digits);
	} else if (digits[1] != '0') {
		zrt_puts(digits + 1);
	} else {
		zrt_puts(digits + 2);
	}
}

void mon_cmd_help(char *cursor)
{
	if (!zrt_line_end(&cursor)) {
		zrt_status_err_arg();
		return;
	}
	zrt_puts(mon_help);
}

void mon_cmd_info(char *cursor)
{
	struct zex_system_info sysinfo;
	struct zex_info info;

	if (!zrt_line_end(&cursor)) {
		zrt_status_err_arg();
		return;
	}
	if (zex_diag_system_info(&sysinfo) != ZEX_DIAG_SYSTEM_INFO_SIZE) {
		zrt_status_err_code(-ZEX_EINVAL);
		return;
	}
	zrt_puts("system: abi ");
	mon_put_hex8(sysinfo.abi);
	zrt_puts(" app ");
	mon_put_hex8(sysinfo.app_mode);
	zrt_puts(" os ");
	mon_put_hex8(sysinfo.os_mode);
	zrt_puts(" common ");
	mon_put_hex16(sysinfo.common_base);
	zrt_puts(" kernel-bank ");
	mon_put_hex8(sysinfo.kernel_bank);
	zrt_puts(" romfs ");
	mon_put_hex8(sysinfo.romfs_first_page);
	zrt_puts(mon_line_end);

	if (zex_getinfo(&info) != ZEX_INFO_SIZE) {
		zrt_status_err_code(-ZEX_EINVAL);
		return;
	}
	zrt_puts("process: pid ");
	mon_put_u8_dec(info.pid);
	zrt_puts(" bank ");
	mon_put_u8_dec(info.bank);
	zrt_puts(" flags ");
	mon_put_hex16(info.flags);
	zrt_puts(mon_line_end);

	zrt_puts("bank: current ");
	mon_put_hex8(mon_bank);
	zrt_puts(" mask ");
	mon_put_hex8(mon_mask);
	zrt_puts(mon_line_end);
}

/* Print the banner once: identity, then the operator scratch address
 * the M/D scratch tests use. */
static void mon_print_banner(void)
{
	struct zex_info info;

	zrt_puts(mon_banner);
	zrt_puts(mon_bank_label);
	mon_put_hex8(mon_bank);
	if (zex_getinfo(&info) == ZEX_INFO_SIZE) {
		zrt_puts(mon_pid_label);
		mon_put_u8_dec(info.pid);
	}
	zrt_puts(mon_scratch_label);
	mon_put_hex16((unsigned int)&mon_scratch);
	zrt_puts(mon_line_end);
}

static void mon_dispatch(char *cursor)
{
	unsigned char command;

	zrt_skip_spaces(&cursor);
	command = (unsigned char)*cursor;
	if (command == '\0') {
		return; /* empty line: just reprompt */
	}
	++cursor;
	if (command >= 'a' && command <= 'z') {
		command = (unsigned char)(command - 'a' + 'A');
	}
	switch (command) {
	case 'H':
	case '?':
		mon_cmd_help(cursor);
		break;
	case 'R':
		mon_cmd_info(cursor);
		break;
	case 'D':
		if (*cursor == 'B' || *cursor == 'b') {
			mon_cmd_mem(cursor + 1, 1, 0);
		} else {
			mon_cmd_mem(cursor, 0, 0);
		}
		break;
	case 'M':
		if (*cursor == 'B' || *cursor == 'b') {
			mon_cmd_mem(cursor + 1, 1, 1);
		} else {
			mon_cmd_mem(cursor, 0, 1);
		}
		break;
	case 'I':
		mon_cmd_io_in(cursor);
		break;
	case 'O':
		mon_cmd_io_out(cursor);
		break;
	case 'Q':
		if (!zrt_line_end(&cursor)) {
			zrt_status_err_arg();
			break;
		}
		zex_exit(MON_EXIT_QUIT);
		break;
	default:
		zrt_status_err_command();
		break;
	}
}

void mon_main(void)
{
	struct zex_bank_info bank_info;

	if (zex_diag_bank_info(&bank_info) != 2) {
		zrt_puts(mon_fail_diag);
		zex_exit(MON_EXIT_DIAG);
	}
	mon_bank = bank_info.bank;
	mon_mask = bank_info.mask;

	mon_print_banner();
	for (;;) {
		int length;

		zrt_puts(mon_prompt);
		length = zrt_read_line(mon_line, sizeof(mon_line));
		if (length < 0) {
			zex_exit(MON_EXIT_CONSOLE);
		}
		mon_dispatch(mon_line);
	}
}
