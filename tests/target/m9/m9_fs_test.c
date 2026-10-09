/* ZephyrOS M9 filesystem syscall acceptance (target, test only).
 *
 * Runs in the SIO-only m9-fs-selftest image after boot, mount and console
 * binding.  It launches the native `fstest.zex` acceptance program through
 * the ordinary process path (the program prints its own per-case results),
 * then verifies the descriptor pool and CWD were released, then launches the
 * small `fstest_l.zex` launcher-cleanup program as a genuine second exec.
 *
 * Diagnostics use the direct early console so the transcript does not depend
 * on which console the boot bound.
 */

#include "zephyros/kernel.h"
#include "zephyros/process.h"

#include "kernel/vfs/vfs.h"

#include "m9_fs_selftest.h"

#if ZOS_M9_FS_SELFTEST

uint8_t m9_fs_selftest_result;

static void m9f_puts(const char *s)
{
	early_console_puts(s);
}

static void m9f_put_hex8(uint8_t value)
{
	static const char hex[] = "0123456789ABCDEF";

	early_console_putc(hex[(value >> 4) & 0x0Fu]);
	early_console_putc(hex[value & 0x0Fu]);
}

static void m9f_fail(uint8_t code)
{
	m9_fs_selftest_result = code;
	m9f_puts("m9 fs selftest: FAIL 0x");
	m9f_put_hex8(code);
	m9f_puts("\r\n");
}

void zos_run_m9_fs_selftest(void)
{
	uint16_t status = 0;
	int result;

	m9_fs_selftest_result = ZOS_M9FST_OK;

	result = process_exec("/SYSTEM/fstest.zex", &status);
	if (result != ZEX_EXEC_OK) {
		m9f_fail(ZOS_M9FST_FAIL_EXEC);
		return;
	}
	if (status != 0u) {
		m9f_fail(ZOS_M9FST_FAIL_STATUS);
		return;
	}
	if (vfs_fd_active_count() != 0u) {
		m9f_fail(ZOS_M9FST_FAIL_LEAK);
		return;
	}

	/* Second exec: its entry environment proves the first program's
	 * descriptors and CWD were released. */
	result = process_exec("/SYSTEM/fstest_l.zex", &status);
	if (result != ZEX_EXEC_OK || status != 0u) {
		m9f_fail(ZOS_M9FST_FAIL_LAUNCHER);
		return;
	}
	if (vfs_fd_active_count() != 0u) {
		m9f_fail(ZOS_M9FST_FAIL_LEAK);
		return;
	}

	m9f_puts("m9 fs selftest: OK\r\n");
}

#endif /* ZOS_M9_FS_SELFTEST */
