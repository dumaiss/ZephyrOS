/* KonaOS VFS part: path normalization. */
/* KonaOS M9 VFS path normalization.
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
		return -KOS_EINVAL;
	}
	if (base[0] != '/') {
		return -KOS_EINVAL;
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
				return -KOS_EINVAL;
			}
			out[olen] = base[olen];
			++olen;
		}
		out[olen] = '\0';
	}
	/* KOS_PATH_MAX counts the terminating NUL: 127 usable bytes. */
	while (n < KOS_PATH_MAX && path[n] != '\0') {
		++n;
	}
	if (n >= KOS_PATH_MAX) {
		return -KOS_EINVAL;
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
		if (clen >= KFS_NAME_MAX) {
			return -KOS_EINVAL; /* component over the limit */
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
			return -KOS_EINVAL;
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

/* --- ASCII helpers ------------------------------------------------------ */

uint8_t vfs_fold(uint8_t c)
{
	return (uint8_t)((c >= 'a' && c <= 'z') ? (c - 32u) : c);
}

/* Case-insensitive equality of a `length`-byte span against the
 * NUL-terminated `word`, which must be exactly `length` bytes long. */
uint8_t vfs_component_eq(const char *p, uint16_t length,
			       const char *word)
{
	uint16_t i;

	for (i = 0; i < length; ++i) {
		if (word[i] == '\0' ||
		    vfs_fold((uint8_t)p[i]) != vfs_fold((uint8_t)word[i])) {
			return 0u;
		}
	}
	return (uint8_t)(word[length] == '\0');
}

/* Length of the first path component of a normalized path fragment that
 * does not begin with '/'. */
uint16_t vfs_component_length(const char *p)
{
	uint16_t length = 0;

	while (p[length] != '\0' && p[length] != '/') {
		++length;
	}
	return length;
}

/* The normalized path names the `root` provider (and no other)? */
uint8_t vfs_path_under(const char *norm, const char *root)
{
	uint8_t length = 0;

	while (root[length] != '\0') {
		if (vfs_fold((uint8_t)norm[length]) !=
		    vfs_fold((uint8_t)root[length])) {
			return 0u;
		}
		++length;
	}
	return (uint8_t)(norm[length] == '\0' || norm[length] == '/');
}
