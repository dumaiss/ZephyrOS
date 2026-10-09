/* M9 integration acceptance runner, split from its cold helpers so the
 * IOC-FAT image retains the production-sized driver arena. */

#include "zephyros/kernel.h"
#include "zephyros/process.h"

#include "kernel/vfs/vfs.h"

#include "m9_integration_selftest.h"
#include "m9_selftest_internal.h"

#if ZOS_M9_SELFTEST

extern uint8_t m9_integration_selftest_result;

void zos_run_m9_selftest(void)
{
	uint16_t status = 0u;
	int mounted;
	int result;

	m9_integration_selftest_result = ZOS_M9IST_OK;
	mounted = m9s_root_mounted();
	if (mounted < 0) {
		m9s_fail(ZOS_M9IST_FAIL_FSTEST);
		return;
	}
	if (mounted == 0) {
		m9s_puts("m9 selftest: OK (card absent)\r\n");
		return;
	}

	/* A read-only card still proves the degraded mutation rule. */
	result = vfs_mkdir(M9S_ROOT);
	if (result == -ZOS_EROFS) {
		m9s_puts("m9 selftest: OK (read-only card)\r\n");
		return;
	}
	if (result != 0 && result != -ZOS_EEXIST) {
		m9s_fail(ZOS_M9IST_FAIL_FSTEST);
		return;
	}

	/* The whole native acceptance, one transcript. */
	if (process_exec("/SYSTEM/fstest.zex", &status) != ZEX_EXEC_OK ||
	    status != 0u) {
		m9s_fail(ZOS_M9IST_FAIL_FSTEST);
		return;
	}
	if (vfs_fd_active_count() != 0u) {
		m9s_fail(ZOS_M9IST_FAIL_LEAK);
		return;
	}

	/* The depth probe: exactly ZOS_EXEC_MAX_DEPTH live levels. */
	if (process_exec("/SYSTEM/fsdeep.zex", &status) != ZEX_EXEC_OK) {
		m9s_fail(ZOS_M9IST_FAIL_EXEC);
		return;
	}
	if (status != (uint16_t)ZOS_EXEC_MAX_DEPTH) {
		m9s_fail(ZOS_M9IST_FAIL_DEPTH);
		return;
	}

	(void)vfs_rmdir(M9S_ROOT);

	if (vfs_fd_active_count() != 0u ||
	    process_active_count() != 0u ||
	    process_free_banks() != PROCESS_BANK_COUNT) {
		m9s_fail(ZOS_M9IST_FAIL_LEAK);
		return;
	}

	m9s_puts("m9 selftest: OK\r\n");
}

#endif
