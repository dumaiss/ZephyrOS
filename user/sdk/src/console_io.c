/* ZephyrOS libzos: console byte/string I/O and bounded line editing.
 *
 * All ordinary I/O goes through the SYS_CONSOLE_* wrappers; nothing here
 * knows which device currently backs the console.  See zos/console.h.
 */

#include "zos.h"

#define ZOS_BS  0x08
#define ZOS_DEL 0x7F
#define ZOS_CR  0x0D
#define ZOS_LF  0x0A
#define ZOS_BEL 0x07

static const char zos_erase_echo[] = "\b \b";
static const char zos_line_end[] = "\r\n";

int zos_putc(char ch)
{
	return zex_write(&ch, 1);
}

int zos_console_write(const char *buf, unsigned int len)
{
	return zex_write(buf, len);
}

int zos_puts(const char *text)
{
	const char *scan = text;
	unsigned int len = 0;

	while (*scan != '\0') {
		++scan;
		++len;
	}
	if (len == 0) {
		return 0;
	}
	return zex_write(text, len);
}

int zos_getc(void)
{
	char ch;

	for (;;) {
		int result = zex_read(&ch, 1);

		if (result == 1) {
			return (int)(unsigned char)ch;
		}
		if (result == 0 || result == -ZOS_EAGAIN) {
			continue; /* nothing pending yet: poll again */
		}
		if (result < 0) {
			return result;
		}
	}
}

int zos_read_line(char *buffer, unsigned int capacity)
{
	static unsigned char cr_pending; /* CR consumed: drop a CRLF's LF */
	unsigned int length = 0;

	if (capacity == 0) {
		return -ZOS_EINVAL;
	}
	for (;;) {
		int ch = zos_getc();
		unsigned char byte;

		if (ch < 0) {
			return ch;
		}
		byte = (unsigned char)ch;

		if (byte == ZOS_LF && cr_pending != 0 && length == 0) {
			cr_pending = 0; /* second half of the previous CRLF */
			continue;
		}
		cr_pending = 0;
		if (byte == ZOS_CR) {
			cr_pending = 1;
			break;
		}
		if (byte == ZOS_LF) {
			break;
		}
		if (byte == ZOS_BS || byte == ZOS_DEL) {
			if (length != 0) {
				--length;
				zos_console_write(zos_erase_echo,
					(unsigned int)(sizeof(zos_erase_echo) - 1u));
			}
			continue;
		}
		if (byte < 0x20) {
			continue; /* other control bytes are ignored */
		}
		if (length >= (capacity - 1u)) {
			zos_putc((char)ZOS_BEL);
			continue;
		}
		buffer[length] = (char)byte;
		++length;
		zos_putc((char)byte);
	}
	buffer[length] = '\0';
	zos_console_write(zos_line_end,
			  (unsigned int)(sizeof(zos_line_end) - 1u));
	return (int)length;
}
