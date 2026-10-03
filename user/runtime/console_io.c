/* ZephyrOS user runtime: console byte/string I/O and bounded line editing.
 *
 * See console_io.h for the contract.  Everything here is ordinary
 * SYS_CONSOLE_* traffic; the blocking read polls the non-blocking
 * SYS_CONSOLE_READ, so no interrupt or device knowledge is needed.
 */

#include "console_io.h"
#include "zex_runtime.h"

#define ZRT_BS  0x08
#define ZRT_DEL 0x7F
#define ZRT_CR  0x0D
#define ZRT_LF  0x0A
#define ZRT_BEL 0x07

static const char zrt_erase_echo[] = "\b \b";
static const char zrt_line_end[] = "\r\n";

int zrt_putc(char ch)
{
	return zex_write(&ch, 1);
}

int zrt_write(const char *buf, unsigned int len)
{
	return zex_write(buf, len);
}

int zrt_puts(const char *text)
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

int zrt_getc(void)
{
	char ch;

	for (;;) {
		int result = zex_read(&ch, 1);

		if (result == 1) {
			return (int)(unsigned char)ch;
		}
		if (result == 0 || result == -ZEX_EAGAIN) {
			continue; /* nothing pending yet: poll again */
		}
		if (result < 0) {
			return result;
		}
	}
}

int zrt_read_line(char *buffer, unsigned int capacity)
{
	static unsigned char cr_pending; /* CR consumed: drop a CRLF's LF */
	unsigned int length = 0;

	if (capacity == 0) {
		return -ZEX_EINVAL;
	}
	for (;;) {
		int ch = zrt_getc();
		unsigned char byte;

		if (ch < 0) {
			return ch;
		}
		byte = (unsigned char)ch;

		if (byte == ZRT_LF && cr_pending != 0 && length == 0) {
			cr_pending = 0; /* second half of the previous CRLF */
			continue;
		}
		cr_pending = 0;
		if (byte == ZRT_CR) {
			cr_pending = 1;
			break;
		}
		if (byte == ZRT_LF) {
			break;
		}
		if (byte == ZRT_BS || byte == ZRT_DEL) {
			if (length != 0) {
				--length;
				zrt_write(zrt_erase_echo,
					  (unsigned int)(sizeof(zrt_erase_echo) - 1u));
			}
			continue;
		}
		if (byte < 0x20) {
			continue; /* other control bytes are ignored */
		}
		if (length >= (capacity - 1u)) {
			zrt_putc((char)ZRT_BEL);
			continue;
		}
		buffer[length] = (char)byte;
		++length;
		zrt_putc((char)byte);
	}
	buffer[length] = '\0';
	zrt_write(zrt_line_end, (unsigned int)(sizeof(zrt_line_end) - 1u));
	return (int)length;
}
