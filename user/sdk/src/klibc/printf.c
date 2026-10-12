/* ZephyrOS zlibc: printf / snprintf.
 *
 * A small output sink backs both entry points: snprintf appends to a
 * bounded caller buffer; printf flushes through the console wrapper in
 * 32-byte chunks.  Supported conversions: %s %c %d %u %x %ld %lu %lx %%
 * with an optional '0' zero-pad flag and a decimal width.
 */

#include "zlibc/fmt.h"
#include "zlibc/string.h"
#include "zlibc/stdarg.h"
#include "zos/console.h"

struct zout {
	char *buf;                 /* NULL selects the console sink */
	unsigned int size;
	unsigned int count;
	char cbuf[32];
	unsigned char clen;
};

static void zout_flush(struct zout *o)
{
	if (o->buf == (char *)0 && o->clen != 0u) {
		(void)console_write(o->cbuf, (unsigned int)o->clen);
		o->clen = 0;
	}
}

static void zout_putc(struct zout *o, char c)
{
	if (o->buf != (char *)0) {
		if (o->count + 1u < o->size) {
			o->buf[o->count] = c;
		}
	} else {
		o->cbuf[o->clen++] = c;
		if (o->clen == sizeof(o->cbuf)) {
			zout_flush(o);
		}
	}
	o->count++;
}

static void zout_str(struct zout *o, const char *s)
{
	while (*s != '\0') {
		zout_putc(o, *s++);
	}
}

static void zout_pad(struct zout *o, unsigned int n, char pad)
{
	while (n-- != 0u) {
		zout_putc(o, pad);
	}
}

static int zfmt(struct zout *o, const char *fmt, va_list ap)
{
	while (*fmt != '\0') {
		char tmp[36];

		if (*fmt != '%') {
			zout_putc(o, *fmt++);
			continue;
		}
		++fmt;
		if (*fmt == '%') {
			zout_putc(o, '%');
			++fmt;
			continue;
		}
		{
			char pad = ' ';
			unsigned char width = 0;

			if (*fmt == '0') {
				pad = '0';
				++fmt;
			}
			while (*fmt >= '0' && *fmt <= '9') {
				width = (unsigned char)(width * 10u +
					(unsigned char)(*fmt - '0'));
				++fmt;
			}
			if (*fmt == 's') {
				const char *s = va_arg(ap, char *);
				unsigned int len;

				if (s == (char *)0) {
					s = "(null)";
				}
				len = strlen(s);
				if (width > len) {
					zout_pad(o, (unsigned int)width - len, ' ');
				}
				zout_str(o, s);
			} else if (*fmt == 'c') {
				char c = (char)va_arg(ap, int);

				if (width > 1u) {
					zout_pad(o, (unsigned int)width - 1u, ' ');
				}
				zout_putc(o, c);
			} else if (*fmt == 'd') {
				itoa(tmp, va_arg(ap, int), 10, width, pad);
				zout_str(o, tmp);
			} else if (*fmt == 'u') {
				utoa(tmp, va_arg(ap, unsigned int), 10,
					 width, pad);
				zout_str(o, tmp);
			} else if (*fmt == 'x') {
				utoa(tmp, va_arg(ap, unsigned int), 16,
					 width, pad);
				zout_str(o, tmp);
			} else if (*fmt == 'l') {
				++fmt;
				if (*fmt == 'd') {
					ltoa(tmp, va_arg(ap, long), 10,
						 width, pad);
					zout_str(o, tmp);
				} else if (*fmt == 'u') {
					ultoa(tmp,
						  va_arg(ap, unsigned long), 10,
						  width, pad);
					zout_str(o, tmp);
				} else if (*fmt == 'x') {
					ultoa(tmp,
						  va_arg(ap, unsigned long), 16,
						  width, pad);
					zout_str(o, tmp);
				} else {
					zout_putc(o, 'l');
					if (*fmt != '\0') {
						zout_putc(o, *fmt);
					}
				}
			} else if (*fmt != '\0') {
				zout_putc(o, *fmt);
			}
		}
		if (*fmt != '\0') {
			++fmt;
		}
	}
	zout_flush(o);
	return (int)o->count;
}

int snprintf(char *dst, unsigned int size, const char *fmt, ...)
{
	struct zout o;
	va_list ap;
	int r;

	o.buf = dst;
	o.size = size;
	o.count = 0;
	o.clen = 0;
	va_start(ap, fmt);
	r = zfmt(&o, fmt, ap);
	va_end(ap);
	if (size != 0u) {
		if (o.count < size) {
			dst[o.count] = '\0';
		} else {
			dst[size - 1u] = '\0';
		}
	}
	return r;
}

int printf(const char *fmt, ...)
{
	struct zout o;
	va_list ap;
	int r;

	o.buf = (char *)0;
	o.size = 0;
	o.count = 0;
	o.clen = 0;
	va_start(ap, fmt);
	r = zfmt(&o, fmt, ap);
	va_end(ap);
	return r;
}
