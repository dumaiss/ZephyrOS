/* ZephyrOS zlibc: freestanding varargs for the SDCC Z80 target.
 *
 * --nostdinc removes the compiler's stdarg.h, so the SDK carries this
 * minimal definition.  On the SDCC Z80 the variadic arguments follow the
 * last named parameter on the stack; va_list is a byte pointer.
 */

#ifndef ZLIBC_STDARG_H
#define ZLIBC_STDARG_H

typedef unsigned char *va_list;

#define va_start(marker, last) { (marker) = (va_list)&(last) + sizeof(last); }
#define va_arg(marker, type) \
	(*((type *)(((marker) += sizeof(type)) - sizeof(type))))
#define va_copy(dest, src) { (dest) = (src); }
#define va_end(marker) { (marker) = (va_list)0; }

#endif /* ZLIBC_STDARG_H */
