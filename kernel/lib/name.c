/* ZephyrOS kernel ASCII name comparison (work package 30c, finding F3).
 *
 * Replaces the three independent copies the kernel used to carry:
 * `vfs_name_equal` (VFS mount and provider names), `romfs_name_match`
 * (bounded ROMFS directory-entry names) and `device_name_equal` (device
 * instance names).  All three keep their exact semantics through the
 * `field_size` and `fold` selectors; see kernel/lib/name.h.
 */

#include "kernel/lib/name.h"

uint8_t zos_ascii_equal(const char *stored, const char *wanted,
			uint16_t field_size, uint8_t fold)
{
	uint16_t i = 0;

	if (stored == (const char *)0 || wanted == (const char *)0) {
		return 0;
	}
	for (;;) {
		uint8_t a;
		uint8_t b;

		if (field_size != 0 && i >= field_size) {
			return 0; /* unterminated bounded field */
		}
		a = (uint8_t)stored[i];
		b = (uint8_t)wanted[i];
		if (fold != 0) {
			if (a >= 'a' && a <= 'z') {
				a = (uint8_t)(a - 32);
			}
			if (b >= 'a' && b <= 'z') {
				b = (uint8_t)(b - 32);
			}
		}
		if (a != b) {
			return 0;
		}
		if (a == '\0') {
			return 1;
		}
		++i;
	}
}
