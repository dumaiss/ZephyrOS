/* ZephyrOS M9 SYS_EXEC child program (test artifact, package 28).
 *
 * Packed as fschild.zex and run by fstest.zex through zos_exec().  It
 * proves, from the child's side:
 *   - it inherited the parent's CWD (printed for the host/simulator);
 *   - no descriptor is inherited (fd 0 is owned by no one);
 *   - its own chdir does not disturb the parent;
 *   - it can open and write a file in the new CWD, then exit with known
 *     status 0x5A5A while leaving the descriptor open, so the parent can
 *     prove the kernel released it;
 *   - SYS_GETINFO reports its true (user) parent, its own slot and its
 *     own application bank.
 *
 * Any unexpected state exits with 0x100 | step so the failure is visible in
 * the parent's status round trip.
 */

#include "zephyros/fs.h"

#include "zex_runtime.h"
#include "zex_fs_runtime.h"

#define CH_EINVAL 0x02

static char ch_cwd[130];

static void ch_puts(const char *s)
{
	unsigned int n = 0;

	while (s[n] != '\0') {
		++n;
	}
	(void)zex_write(s, n);
}

static void ch_fail(unsigned char step)
{
	ch_puts("fschild: FAIL\r\n");
	zex_exit((unsigned int)(0x0100u | step));
}

void ch_main(void)
{
	int r;
	int fd;

	r = zos_getcwd(ch_cwd, (unsigned int)sizeof(ch_cwd));
	if (r < 0) {
		ch_fail(0x01u);
	}
	ch_puts("fschild: cwd ");
	(void)zex_write(ch_cwd, (unsigned int)r);
	ch_puts("\r\n");

	/* Descriptors are not inherited: fd 0 belongs to no one. */
	if (zos_close(0u) != -(int)CH_EINVAL) {
		ch_fail(0x02u);
	}

	/* The child's chdir must not disturb the parent. */
	if (zos_chdir("/R2") != 0) {
		ch_fail(0x03u);
	}

	fd = zos_open("CHILD.TXT",
		      (unsigned char)(ZFS_O_WRITE | ZFS_O_CREATE |
				      ZFS_O_TRUNC));
	if (fd < 0) {
		ch_fail(0x04u);
	}
	if (zos_write((unsigned int)fd, "child", 5u) != 5) {
		ch_fail(0x05u);
	}

	/* SYS_GETINFO reports the true parent: this child was started by an
	 * ordinary process, so the kernel-parent flag is clear. */
	{
		struct zex_info info;

		if (zex_getinfo(&info) < 0 ||
		    info.flags != 0 ||
		    info.pid > 6u || info.bank > 6u ||
		    info.bank == 0u) {
			ch_fail(0x06u);
		}
	}
	/* Deliberately no close: the kernel must release the descriptor and
	 * the provider handle at exit. */
	zex_exit(0x5A5Au);
}
