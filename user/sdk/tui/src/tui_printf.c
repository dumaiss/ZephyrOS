/* libtui formatted text.
 *
 * The formatter is a separate archive member so a program that never
 * calls tui_printf()/tui_printf_xy() links none of it.  It supports the
 * same conversions and width rules as the SDK's small printf:
 * %s %c %d %u %x %ld %lu %lx %% with an optional leading '0' and a
 * decimal width.  Output goes through the ordinary text cursor (or a
 * caller-supplied position for the _xy form); there is no heap and no
 * fixed output buffer.
 */

#include "tui_internal.h"
#include "klibc/fmt.h"
#include "klibc/stdarg.h"
#include "klibc/string.h"

struct tui_format_out {
	uint8_t x;
	uint8_t y;
	int count;
};

static void tui_format_putc(struct tui_format_out *out, char ch)
{
	tui_priv_put_byte(&out->x, &out->y, (uint8_t)ch);
	++out->count;
}

static void tui_format_str(struct tui_format_out *out, const char *text)
{
	while (*text != '\0') {
		tui_format_putc(out, *text);
		++text;
	}
}

static void tui_format_pad(struct tui_format_out *out, unsigned int count,
			   char pad)
{
	while (count-- != 0u) {
		tui_format_putc(out, pad);
	}
}

static int tui_format(struct tui_format_out *out, const char *format,
		      va_list ap)
{
	while (*format != '\0') {
		char tmp[36];

		if (*format != '%') {
			tui_format_putc(out, *format);
			++format;
			continue;
		}
		++format;
		if (*format == '%') {
			tui_format_putc(out, '%');
			++format;
			continue;
		}
		{
			char pad = ' ';
			unsigned char width = 0;

			if (*format == '0') {
				pad = '0';
				++format;
			}
			while (*format >= '0' && *format <= '9') {
				width = (unsigned char)(width * 10u +
					(unsigned char)(*format - '0'));
				++format;
			}
			if (*format == 's') {
				const char *text = va_arg(ap, char *);
				unsigned int length;

				if (text == (char *)0) {
					text = "(null)";
				}
				length = strlen(text);
				if (width > length) {
					tui_format_pad(out,
						(unsigned int)width - length,
						' ');
				}
				tui_format_str(out, text);
			} else if (*format == 'c') {
				char ch = (char)va_arg(ap, int);

				if (width > 1u) {
					tui_format_pad(out,
						(unsigned int)width - 1u,
						' ');
				}
				tui_format_putc(out, ch);
			} else if (*format == 'd') {
				itoa(tmp, va_arg(ap, int), 10, width, pad);
				tui_format_str(out, tmp);
			} else if (*format == 'u') {
				utoa(tmp, va_arg(ap, unsigned int), 10,
				     width, pad);
				tui_format_str(out, tmp);
			} else if (*format == 'x') {
				utoa(tmp, va_arg(ap, unsigned int), 16,
				     width, pad);
				tui_format_str(out, tmp);
			} else if (*format == 'l') {
				++format;
				if (*format == 'd') {
					ltoa(tmp, va_arg(ap, long), 10,
					     width, pad);
					tui_format_str(out, tmp);
				} else if (*format == 'u') {
					ultoa(tmp,
					      va_arg(ap, unsigned long), 10,
					      width, pad);
					tui_format_str(out, tmp);
				} else if (*format == 'x') {
					ultoa(tmp,
					      va_arg(ap, unsigned long), 16,
					      width, pad);
					tui_format_str(out, tmp);
				} else {
					tui_format_putc(out, 'l');
					if (*format != '\0') {
						tui_format_putc(out, *format);
					}
				}
			} else if (*format != '\0') {
				tui_format_putc(out, *format);
			}
		}
		if (*format != '\0') {
			++format;
		}
	}
	return out->count;
}

int tui_printf(const char *format, ...)
{
	struct tui_format_out out;
	va_list ap;
	int result;

	if (tui_state.initialized == 0u) {
		return -KOS_EINVAL;
	}
	if (format == (const char *)0) {
		return -KOS_EINVAL;
	}
	out.x = tui_state.cursor_x;
	out.y = tui_state.cursor_y;
	out.count = 0;
	va_start(ap, format);
	result = tui_format(&out, format, ap);
	va_end(ap);
	tui_state.cursor_x = out.x;
	tui_state.cursor_y = out.y;
	return result;
}

int tui_printf_xy(uint8_t x, uint8_t y, const char *format, ...)
{
	struct tui_format_out out;
	va_list ap;
	int result;

	if (tui_state.initialized == 0u) {
		return -KOS_EINVAL;
	}
	if (format == (const char *)0) {
		return -KOS_EINVAL;
	}
	out.x = x;
	out.y = y;
	out.count = 0;
	va_start(ap, format);
	result = tui_format(&out, format, ap);
	va_end(ap);
	return result;
}
