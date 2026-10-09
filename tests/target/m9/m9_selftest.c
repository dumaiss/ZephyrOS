/* ZephyrOS M9 integration acceptance (target, test only, package 31).
 *
 * Runs in the SIO-console `m9-selftest` image after boot, mount and console
 * binding.  The production `config/mounts.json` attaches `/` to the resident
 * `iocfat` provider (optional), so with a card this image proves the whole
 * milestone end to end against real media; with no card, and with a
 * write-protected card, it proves the documented degraded paths.
 *
 * Steps:
 *   1. exec the native `fstest.zex` acceptance program.  It runs the public
 *      FS_* and SYS_EXEC families and exec's `fsfat.zex`, whose deep matrix
 *      (namespace, TRUNC/sync/seek/stat/rename, the 1..513 and >64 KiB
 *      size set compared by an in-program check value, cache data
 *      correctness, EMFILE, EXDEV) is too large for one ZEX image.
 *      `fsfat.zex` also drives the two section-33 error paths that cannot
 *      be produced deterministically on demand -- a stale backend handle
 *      and a failed confirmed write.  The simulator arms each fault on its
 *      `arm` fence; hardware records the matching `*:fresh` note when the
 *      ordinary operation succeeds.
 *   2. exec the recursive `fsdeep.zex` depth probe: its exit status is
 *      exactly ZOS_EXEC_MAX_DEPTH, so the level past the limit was refused
 *      with ZOS_EBUSY.
 *   3. check the descriptor pool, the process table and the bank
 *      reservations are back to baseline.
 *
 * Diagnostics use the direct early console so the transcript does not
 * depend on which console the boot bound.
 */

#include "zephyros/kernel.h"
#include "zephyros/process.h"

#include "kernel/lib/name.h"
#include "kernel/vfs/vfs.h"

#include "m9_integration_selftest.h"
#include "m9_selftest_internal.h"

#if ZOS_M9_SELFTEST

uint8_t m9_integration_selftest_result;

void m9s_puts(const char *s)
{
	early_console_puts(s);
}

void m9s_hex8(uint8_t value)
{
	static const char hex[] = "0123456789ABCDEF";

	early_console_putc(hex[(value >> 4) & 0x0Fu]);
	early_console_putc(hex[value & 0x0Fu]);
}

void m9s_fail(uint8_t code)
{
	m9_integration_selftest_result = code;
	m9s_puts("m9 selftest: FAIL 0x");
	m9s_hex8(code);
	m9s_puts("\r\n");
}

/* 1 = `/` mounted, 0 = the documented absent-media path, -1 = anything
 * else. */
int m9s_root_mounted(void)
{
	uint16_t i;

	for (i = 0; i < vfs_mount_count; ++i) {
		if (zos_ascii_equal(vfs_mount_table[i].path, "/", 0u, 1u)) {
			if (vfs_mount_state[i].state == VFS_MOUNT_MOUNTED) {
				return 1;
			}
			if (vfs_mount_state[i].state == VFS_MOUNT_FAILED &&
			    vfs_mount_state[i].last_error == -ZOS_ENODEV) {
				return 0;
			}
			return -1;
		}
	}
	return -1;
}

#endif /* ZOS_M9_SELFTEST */
