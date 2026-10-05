/* ZephyrOS M9 launcher-cleanup acceptance program (test artifact).
 *
 * Exec'd a second time, after fstest.zex exits, to prove the global
 * descriptor pool and the CWD were released: a clean entry has CWD `/` and
 * the whole 8-entry VFS pool free.  The provider-open-handle pools are
 * smaller than 8, so the pool is filled by combining the three mounted
 * filesystems (4 handles in `/`, 2 in `/SYSTEM`, 2 in `/R2`); the ninth
 * open must be EMFILE from the VFS pool.  Everything is closed again.
 * Prints `fstest: launcher OK` (or FAIL).
 */

#include "zos.h"
static void ftl_puts(const char *s)
{
	unsigned int n = 0;

	while (s[n] != '\0') {
		++n;
	}
	(void)zex_write(s, n);
}

int main(void)
{
	char cwd[8];
	int fds[8];
	unsigned int i;
	int ok = 1;
	int ninth;

	if (zos_getcwd(cwd, sizeof(cwd)) != 1 || cwd[0] != '/' ||
	    cwd[1] != '\0') {
		ok = 0;
	}

	fds[0] = zos_open("/P.BIN", (unsigned char)(ZFS_O_WRITE |
						    ZFS_O_CREATE |
						    ZFS_O_TRUNC));
	fds[1] = zos_open("/P.BIN", ZFS_O_READ);
	fds[2] = zos_open("/P.BIN", ZFS_O_READ);
	fds[3] = zos_open("/P.BIN", ZFS_O_READ);
	fds[4] = zos_open("/SYSTEM/fstest.zex", ZFS_O_READ);
	fds[5] = zos_open("/SYSTEM/fstest.zex", ZFS_O_READ);
	fds[6] = zos_open("/R2/Q.BIN", (unsigned char)(ZFS_O_WRITE |
						       ZFS_O_CREATE |
						       ZFS_O_TRUNC));
	fds[7] = zos_open("/R2/Q.BIN", ZFS_O_READ);
	for (i = 0; i < 8u; ++i) {
		if (fds[i] < 0) {
			ok = 0;
		}
	}
	ninth = zos_open("/SYSTEM/fstest.zex", ZFS_O_READ);
	if (ok && (ninth + (int)ZOS_EMFILE) != 0) {
		ok = 0;
	}
	for (i = 0; i < 8u; ++i) {
		if (fds[i] >= 0) {
			(void)zos_close((unsigned int)fds[i]);
		}
	}
	(void)zos_unlink("/P.BIN");
	(void)zos_unlink("/R2/Q.BIN");
	ftl_puts(ok ? "fstest: launcher OK\r\n" : "fstest: launcher FAIL\r\n");
	zex_exit((unsigned int)(ok ? 0u : 1u));
	return 0; /* unreachable */
}
