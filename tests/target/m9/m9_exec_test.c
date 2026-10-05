/* ZephyrOS M9 SYS_EXEC acceptance (target, test only, package 28).
 *
 * Runs in the SIO-only m9-exec-selftest image after boot and mount.  It
 * launches the extended `fstest.zex` (whose `exec` case runs the child,
 * descriptor, status and failure-mode checks from a user process), then the
 * recursive `fsdeep.zex` depth probe.  fsdeep exits with the number of live
 * levels; exactly ZOS_EXEC_MAX_DEPTH proves the fifth exec was refused with
 * ZOS_EBUSY.  Afterwards the process table, bank map and descriptor pool
 * must be back to baseline.
 *
 * Diagnostics use the direct early console.
 */

#include "zephyros/kernel.h"
#include "zephyros/process.h"
#include "zephyros/console.h"

#include "kernel/vfs/vfs.h"

#include "m9_exec_selftest.h"

#if ZOS_M9_EXEC_SELFTEST

uint8_t m9_exec_selftest_result;

static void m9e_puts(const char *s)
{
	early_console_puts(s);
}

static void m9e_put_hex8(uint8_t value)
{
	static const char hex[] = "0123456789ABCDEF";

	early_console_putc(hex[(value >> 4) & 0x0Fu]);
	early_console_putc(hex[value & 0x0Fu]);
}

static void m9e_fail(uint8_t code)
{
	m9_exec_selftest_result = code;
	m9e_puts("m9 exec selftest: FAIL 0x");
	m9e_put_hex8(code);
	m9e_puts("\r\n");
}

void zos_run_m9_exec_selftest(void)
{
	uint16_t status = 0;
	int result;

	m9_exec_selftest_result = ZOS_M9EST_OK;

	/* The extended FS program: its `exec` case covers CWD inheritance, a
	 * child chdir, descriptor release, the status round trip, a raw-return
	 * EFAULT, every failure mode and 32 exec/exit cycles. */
	result = process_exec("/SYSTEM/fstest.zex", &status);
	if (result != ZEX_EXEC_OK || status != 0u) {
		m9e_fail(ZOS_M9EST_FAIL_EXEC);
		return;
	}

	/* Depth probe: from the kernel launcher (depth 0) the chain must reach
	 * exactly ZOS_EXEC_MAX_DEPTH live user processes, and the refusal must
	 * leave nothing allocated. */
	result = process_exec("/SYSTEM/fsdeep.zex", &status);
	if (result != ZEX_EXEC_OK) {
		m9e_fail(ZOS_M9EST_FAIL_EXEC);
		return;
	}
	if (status != (uint16_t)ZOS_EXEC_MAX_DEPTH) {
		m9e_fail(ZOS_M9EST_FAIL_DEPTH);
		return;
	}

	if (vfs_fd_active_count() != 0u ||
	    process_active_count() != 0u ||
	    process_free_banks() != PROCESS_BANK_COUNT) {
		m9e_fail(ZOS_M9EST_FAIL_LEAK);
		return;
	}

	m9e_puts("m9 exec selftest: OK\r\n");
}

#endif /* ZOS_M9_EXEC_SELFTEST */
