/* ZephyrOS M9 VFS path normalization and native name comparison.
 *
 * Absolute/relative paths, `.`, `..`, repeated slashes and traversal above
 * `/` are handled here.  Comparison is ASCII case-insensitive (A-Z == a-z,
 * every other byte exact); ZephyrOS performs no Unicode folding.  No state
 * is kept: the caller provides the output buffer, so the function is safe
 * from both the boot stack and the gate stack.
 */

#include "kernel/vfs/vfs.h"

int vfs_path_normalize(const char *path, char *out, uint16_t out_size)
{
	uint16_t olen = 1;
	uint16_t i = 0;
	uint16_t n = 0;

	if (path == (const char *)0 || out == (char *)0 || out_size < 2u) {
		return -ZOS_EINVAL;
	}
	/* ZOS_PATH_MAX counts the terminating NUL: 127 usable bytes. */
	while (n < ZOS_PATH_MAX && path[n] != '\0') {
		++n;
	}
	if (n >= ZOS_PATH_MAX) {
		return -ZOS_EINVAL;
	}

	out[0] = '/';
	out[1] = '\0';

	while (path[i] != '\0') {
		uint16_t start;
		uint16_t clen;

		while (path[i] == '/') {
			++i;
		}
		if (path[i] == '\0') {
			break;
		}
		start = i;
		while (path[i] != '\0' && path[i] != '/') {
			++i;
		}
		clen = (uint16_t)(i - start);
		if (clen >= ZFS_NAME_MAX) {
			return -ZOS_EINVAL; /* component over the limit */
		}
		if (clen == 1u && path[start] == '.') {
			continue;
		}
		if (clen == 2u && path[start] == '.' &&
		    path[start + 1] == '.') {
			if (olen > 1u) {
				uint16_t k = olen;

				while (k > 1u && out[k - 1u] != '/') {
					--k;
				}
				olen = (uint16_t)((k > 1u) ? (k - 1u) : 1u);
				out[olen] = '\0';
			}
			continue; /* at the root, `..` clamps to `/` */
		}
		if ((uint32_t)olen + (uint32_t)clen + 1u > (uint32_t)out_size) {
			return -ZOS_EINVAL;
		}
		if (olen > 1u) {
			out[olen++] = '/';
		}
		{
			uint16_t k;

			for (k = 0; k < clen; ++k) {
				out[olen++] = path[start + k];
			}
		}
		out[olen] = '\0';
	}
	return 0;
}

uint8_t vfs_name_equal(const char *a, const char *b)
{
	if (a == (const char *)0 || b == (const char *)0) {
		return 0;
	}
	for (;;) {
		uint8_t ca = (uint8_t)*a++;
		uint8_t cb = (uint8_t)*b++;

		if (ca >= 'a' && ca <= 'z') {
			ca = (uint8_t)(ca - 32);
		}
		if (cb >= 'a' && cb <= 'z') {
			cb = (uint8_t)(cb - 32);
		}
		if (ca != cb) {
			return 0;
		}
		if (ca == '\0') {
			return 1;
		}
	}
}
