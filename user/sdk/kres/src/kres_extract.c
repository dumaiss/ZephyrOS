/* KRES core: kres_extract (private).
 *
 * Copies one resource's bytes into a new file at `path`, through the
 * platform's own file facility: the C stdio on the host, the SDK file
 * API on the target.  The container's storage itself is reached only
 * through the installed adapter.
 */

#include "kres_internal.h"

#if defined(__SDCC)

#include "kos.h"
#include "kos/fs.h"

int kres_extract(kres_container *c, const char *name, const char *path)
{
	kres_resource r;
	unsigned int handle;
	int result;

	result = kres_get(c, name, &r, KRES_OPEN_READ);
	if (result < 0) {
		return result;
	}
	handle = (unsigned int)open(path, KFS_O_WRITE | KFS_O_CREATE |
					KFS_O_TRUNC);
	if ((int)handle < 0) {
		(void)kres_rclose(&r);
		return (int)handle;
	}
	result = 0;
	for (;;) {
		int got = kres_read(&r, kres_shared, KRES_RECORD_SIZE);

		if (got < 0) {
			result = got;
			break;
		}
		if (got == 0) {
			break;
		}
		if (write(handle, kres_shared, (unsigned int)got) != got) {
			result = -KOS_EIO;
			break;
		}
	}
	if (close(handle) < 0 && result == 0) {
		result = -KOS_EIO;
	}
	(void)kres_rclose(&r);
	return result;
}

#else /* !__SDCC: the host */

#include <stdio.h>

int kres_extract(kres_container *c, const char *name, const char *path)
{
	kres_resource r;
	FILE *out;
	int result;

	result = kres_get(c, name, &r, KRES_OPEN_READ);
	if (result < 0) {
		return result;
	}
	out = fopen(path, "wb");
	if (out == (FILE *)0) {
		(void)kres_rclose(&r);
		return -KOS_EIO;
	}
	result = 0;
	for (;;) {
		int got = kres_read(&r, kres_shared, KRES_RECORD_SIZE);

		if (got < 0) {
			result = got;
			break;
		}
		if (got == 0) {
			break;
		}
		if (fwrite(kres_shared, 1u, (size_t)got, out) != (size_t)got) {
			result = -KOS_EIO;
			break;
		}
	}
	if (fclose(out) != 0 && result == 0) {
		result = -KOS_EIO;
	}
	(void)kres_rclose(&r);
	return result;
}

#endif /* __SDCC */
