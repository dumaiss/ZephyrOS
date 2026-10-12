/* KonaOS M7 native monitor: startup, command input, info/help
 * commands, dispatch loop.
 *
 * See monitor.h for the command surface.  The monitor resolves its own
 * bank and the platform bank mask once at startup (SYS_DIAG_BANK_INFO),
 * prints a banner that includes a safe operator scratch address, then
 * dispatches edited 80-character command lines until Q exits through
 * SYS_EXIT.  Commands come from the console and, when the store
 * publishes a serial object at the lane name monitor.h documents, from
 * that lane as well; the reader and its echo routing are below.
 */

#include "monitor.h"
#include "kos.h"
#include "diag.h" /* the SDK diag wrappers */
#include "hexfmt.h"
#include "status.h"
#include "kos/protocol.h"

unsigned char mon_bank;
unsigned char mon_mask;
unsigned char mon_line[MON_LINE_MAX];

/* Safe one-byte operator scratch inside this program's BSS: `M`/`D` on
 * its address (printed in the banner) exercise the diagnostic memory
 * path without touching live monitor state. */
static unsigned char mon_scratch;

static const char mon_banner[] = "\r\nKonaOS native monitor\r\n";
static const char mon_prompt[] = "Z80> ";
static const char mon_bank_label[] = "bank=";
static const char mon_pid_label[] = " pid=";
static const char mon_scratch_label[] = " scratch=";
static const char mon_line_end[] = "\r\n";
static const char mon_fail_diag[] =
	"monitor: diagnostic drivers unavailable\r\n";

static const char mon_help[] =
	"R              system/bank information\r\n"
	"D addr len     dump monitor bank (16-byte rows)\r\n"
	"DB bank addr len  dump bank 0-6\r\n"
	"M addr value   set byte in monitor bank\r\n"
	"MB bank addr value set byte in bank 0-6\r\n"
	"I port         input port (10-FF)\r\n"
	"O port value   output port (10-FF)\r\n"
	"X path [tail]  run a EXE from /system\r\n"
	"Q              quit\r\n"
	"H or ?         help\r\n";

/* --- command input: the console plus the optional secondary lane --------
 *
 * The secondary lane is the namespace name a resident serial driver
 * publishes.  The monitor binds it through the ordinary typed protocol -
 * no port I/O and no driver symbol - and reads and echoes it only when
 * the store actually publishes that object.  Production's console input
 * is the HID build, so the lane is the machine-readable command path
 * there (the smoke tool, or a terminal on the lane) while the console
 * keeps working unchanged.  With no such object the monitor is
 * console-only, exactly as monitor.h documents.
 *
 * The console is polled first: in the serial-console build both sources
 * show the same stream, so the first poll consumes each byte and the lane
 * path stays idle.  Each byte is echoed to the source it came from, so
 * that build still renders every typed byte exactly once.
 */
#define MON_BS  0x08
#define MON_DEL 0x7F
#define MON_CR  0x0D
#define MON_LF  0x0A
#define MON_BEL 0x07
#define MON_CTRL_TOP 0x1F /* last non-printable byte */

static const char mon_lane_name[] = "/dev/serial/0";
static const char mon_erase_echo[] = "\b \b";

static unsigned char mon_lane;      /* lane handle, 0 when absent        */
static unsigned char mon_lane_byte; /* 1: the current byte came from it  */

/* One descriptor-free call on the lane. */
static int mon_lane_call(unsigned char op, unsigned int arg0)
{
	struct kos_ipc_request req;

	req.frame.op = op;
	req.frame.flags = 0u;
	req.frame.arg0 = arg0;
	req.frame.arg1 = 0u;
	req.buffer = (void *)0;
	req.length = 0u;
	req.direction = KOS_IPC_DESC_IN;
	return call(mon_lane, &req);
}

/* Bind the lane when the store publishes a SERIAL object under that name. */
static void mon_lane_open(void)
{
	unsigned char type = 0u;
	int handle = bind(mon_lane_name, 0u, &type);

	if (handle >= 1 && type == KOS_TYPE_SERIAL) {
		mon_lane = (unsigned char)handle;
	}
}

/* One pending lane byte, or -KOS_EAGAIN.  A lane that fails is dropped
 * for the rest of the session: the console remains. */
static int mon_lane_getc(void)
{
	int result;

	if (mon_lane == 0u) {
		return -KOS_EAGAIN;
	}
	result = mon_lane_call(KSIO_OP_POLL, 0u);
	if (result != 1) {
		if (result < 0) {
			mon_lane = 0u;
		}
		return -KOS_EAGAIN;
	}
	result = mon_lane_call(KSIO_OP_GETC, 0u);
	if (result < 0) {
		if (result != -KOS_EAGAIN) {
			mon_lane = 0u;
		}
		return -KOS_EAGAIN;
	}
	return result;
}

static void mon_lane_putc(unsigned int byte)
{
	if (mon_lane != 0u && mon_lane_call(KSIO_OP_PUTC, byte) < 0) {
		mon_lane = 0u;
	}
}

/* Write one byte / a short buffer back to the source of the line. */
static void mon_echo_putc(unsigned int byte)
{
	if (mon_lane_byte != 0u) {
		mon_lane_putc(byte);
		return;
	}
	(void)putchar((char)byte);
}

static void mon_echo_write(const char *text, unsigned int length)
{
	unsigned int i;

	if (mon_lane_byte == 0u) {
		(void)console_write(text, length);
		return;
	}
	for (i = 0u; i < length; ++i) {
		mon_lane_putc((unsigned char)text[i]);
	}
}

/* Blocking byte read over both sources: the console read is the
 * non-blocking drain, so the lane is polled on every turn and a waiting
 * command on either source is served. */
static int mon_getc(void)
{
	for (;;) {
		char ch;
		int result = console_read(&ch, 1);

		if (result == 1) {
			mon_lane_byte = 0u;
			return (int)(unsigned char)ch;
		}
		if (result < 0 && result != -KOS_EAGAIN) {
			return result;
		}
		result = mon_lane_getc();
		if (result >= 0) {
			mon_lane_byte = 1u;
			return result;
		}
	}
}

/* The edited line reader: the rules of the SDK's console_readline (BS/DEL
 * erase echo, CR/LF termination with CRLF suppression, bell instead of
 * overflow, other control bytes ignored, capacity-1 characters), with
 * the two sources above and the echo routed to the source. */
static int mon_read_line(char *buffer, unsigned int capacity)
{
	static unsigned char cr_pending;
	unsigned int length = 0;

	for (;;) {
		int ch = mon_getc();
		unsigned char byte;

		if (ch < 0) {
			return ch;
		}
		byte = (unsigned char)ch;
		if (byte == MON_LF && cr_pending != 0 && length == 0) {
			cr_pending = 0; /* second half of the previous CRLF */
			continue;
		}
		cr_pending = 0;
		if (byte == MON_CR) {
			cr_pending = 1;
			break;
		}
		if (byte == MON_LF) {
			break;
		}
		if (byte == MON_BS || byte == MON_DEL) {
			if (length != 0) {
				--length;
				mon_echo_write(mon_erase_echo,
					(unsigned int)(sizeof(mon_erase_echo) - 1u));
			}
			continue;
		}
		if (byte <= MON_CTRL_TOP) {
			continue; /* other control bytes are ignored */
		}
		if (length >= (capacity - 1u)) {
			mon_echo_putc(MON_BEL);
			continue;
		}
		buffer[length] = (char)byte;
		++length;
		mon_echo_putc(byte);
	}
	buffer[length] = '\0';
	mon_echo_write(mon_line_end,
		(unsigned int)(sizeof(mon_line_end) - 1u));
	return (int)length;
}

void mon_put_hex8(unsigned int value)
{
	char digits[3];

	zrt_fmt_hex8(digits, value);
	console_puts(digits);
}

void mon_put_hex16(unsigned int value)
{
	char digits[5];

	zrt_fmt_hex16(digits, value);
	console_puts(digits);
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
		console_puts(digits);
	} else if (digits[1] != '0') {
		console_puts(digits + 1);
	} else {
		console_puts(digits + 2);
	}
}

void mon_cmd_help(char *cursor)
{
	if (!zrt_line_end(&cursor)) {
		zrt_status_err_arg();
		return;
	}
	console_puts(mon_help);
}

void mon_cmd_info(char *cursor)
{
	struct kos_diag_system_info sysinfo;
	struct kos_info_record info;

	if (!zrt_line_end(&cursor)) {
		zrt_status_err_arg();
		return;
	}
	if (diag_system_info(&sysinfo) != KOS_DIAG_SYSTEM_INFO_SIZE) {
		zrt_status_err_code(-KOS_EINVAL);
		return;
	}
	console_puts("system: abi ");
	mon_put_hex8(sysinfo.abi);
	console_puts(" app ");
	mon_put_hex8(sysinfo.app_mode);
	console_puts(" os ");
	mon_put_hex8(sysinfo.os_mode);
	console_puts(" common ");
	mon_put_hex16(sysinfo.common_base);
	console_puts(" kernel-bank ");
	mon_put_hex8(sysinfo.kernel_bank);
	console_puts(" romfs ");
	mon_put_hex8(sysinfo.romfs_first_page);
	console_puts(mon_line_end);

	if (getinfo(&info) != KOS_GETINFO_SIZE) {
		zrt_status_err_code(-KOS_EINVAL);
		return;
	}
	console_puts("process: pid ");
	mon_put_u8_dec(info.pid);
	console_puts(" bank ");
	mon_put_u8_dec(info.bank);
	console_puts(" flags ");
	mon_put_hex16(info.flags);
	console_puts(mon_line_end);

	console_puts("bank: current ");
	mon_put_hex8(mon_bank);
	console_puts(" mask ");
	mon_put_hex8(mon_mask);
	console_puts(mon_line_end);
}

/* Print the banner once: identity, then the operator scratch address
 * the M/D scratch tests use. */
static void mon_print_banner(void)
{
	struct kos_info_record info;

	console_puts(mon_banner);
	console_puts(mon_bank_label);
	mon_put_hex8(mon_bank);
	if (getinfo(&info) == KOS_GETINFO_SIZE) {
		console_puts(mon_pid_label);
		mon_put_u8_dec(info.pid);
	}
	console_puts(mon_scratch_label);
	mon_put_hex16((unsigned int)&mon_scratch);
	console_puts(mon_line_end);
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
	case 'X':
		mon_cmd_exec(cursor);
		break;
	case 'Q':
		if (!zrt_line_end(&cursor)) {
			zrt_status_err_arg();
			break;
		}
		exit(MON_EXIT_QUIT);
		break;
	default:
		zrt_status_err_command();
		break;
	}
}

int main(void)
{
	struct kos_bank_info bank_info;

	if (diag_bank_info(&bank_info) != 2) {
		console_puts(mon_fail_diag);
		exit(MON_EXIT_DIAG);
	}
	mon_bank = bank_info.bank;
	mon_mask = bank_info.mask;

	mon_lane_open();
	mon_print_banner();
	for (;;) {
		int length;

		console_puts(mon_prompt);
		length = mon_read_line(mon_line, sizeof(mon_line));
		if (length < 0) {
			exit(MON_EXIT_CONSOLE);
		}
		mon_dispatch(mon_line);
	}
}
