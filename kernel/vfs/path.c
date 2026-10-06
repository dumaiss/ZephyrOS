/* ZephyrOS M9 VFS path normalization.
 *
 * Absolute/relative paths, `.`, `..`, repeated slashes and traversal above
 * `/` are handled here.  No state is kept: the caller provides the output
 * buffer, so the function is safe from both the boot stack and the gate
 * stack.  Name comparison lives in one shared kernel helper
 * (`kernel/lib/name.c`).
 */

#include "kernel/vfs/vfs.h"

int vfs_path_normalize_at(const char *base, const char *path, char *out,
			  uint16_t out_size)
{
	uint16_t olen;
	uint16_t i = 0;
	uint16_t n = 0;

	if (base == (const char *)0 || path == (const char *)0 ||
	    out == (char *)0 || out_size < 2u) {
		return -ZOS_EINVAL;
	}
	if (base[0] != '/') {
		return -ZOS_EINVAL;
	}
	/* A relative path resolves against `base`; an absolute one ignores
	 * it, exactly like a filesystem root. */
	if (path[0] == '/') {
		out[0] = '/';
		out[1] = '\0';
		olen = 1;
	} else {
		olen = 0;
		while (base[olen] != '\0') {
			if (olen + 1u >= out_size) {
				return -ZOS_EINVAL;
			}
			out[olen] = base[olen];
			++olen;
		}
		out[olen] = '\0';
	}
	/* ZOS_PATH_MAX counts the terminating NUL: 127 usable bytes. */
	while (n < ZOS_PATH_MAX && path[n] != '\0') {
		++n;
	}
	if (n >= ZOS_PATH_MAX) {
		return -ZOS_EINVAL;
	}
	if (n == 0) {
		return 0; /* empty relative path is the base itself */
	}

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
