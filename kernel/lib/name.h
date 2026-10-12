/* KonaOS kernel name-comparison interface (kernel/lib).
 *
 * One comparison for every kernel-side ASCII name: VFS mount/provider
 * names, ROMFS names and device instance names.  It is kernel-private and
 * never module-facing: a MOD may not import kernel symbols, so the
 * resident modules keep their own copies by isolation (work package 30c,
 * finding F3).
 */

#ifndef KONAOS_KERNEL_LIB_NAME_H
#define KONAOS_KERNEL_LIB_NAME_H

#include "kos/types.h"

/* Return nonzero when `stored` and `wanted` are the same ASCII name.
 *
 * `field_size` selects the stored-name form.  When it is nonzero,
 * `stored` is a fixed-size field and at most that many bytes are
 * examined, so a bounded name (a ROMFS directory entry) is never read
 * past its width and an unterminated field cannot match; when it is
 * zero, `stored` and `wanted` are ordinary NUL-terminated strings.
 *
 * `fold` selects ASCII case folding (A-Z == a-z) when nonzero; zero
 * compares the bytes exactly.  Every caller's own semantics are kept:
 * VFS and ROMFS fold, the device registry does not.
 *
 * A null argument is never equal. */
uint8_t kos_ascii_equal(const char *stored, const char *wanted,
			uint16_t field_size, uint8_t fold);

#endif /* KONAOS_KERNEL_LIB_NAME_H */
