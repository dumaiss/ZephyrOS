/* ZephyrOS process/ZEX acceptance selftest (test artifact).
 *
 *   zos_run_process_selftest()          subsystem image (ZOS_SUBSYS_SELFTEST)
 *     - zextest runs from ROMFS in bank 0 and exits with PASS|0;
 *     - repeated exec/exit cycles leak no slot or bank;
 *     - reserving bank 0 through the tested allocator makes the same
 *       ROMFS payload run in bank 1 (PASS|1);
 *     - a raw entry return exits with ZOS_EFAULT through the common
 *       trap;
 *     - every malformed fixture is rejected with its exact code and
 *       leaves the table clean;
 *     - argument errors and allocator exhaustion are precise;
 *     - bank 7 stays unreachable through the normal process API;
 *     - a truncated container in the store is rejected with
 *       ZEX_EXEC_ERR_FILESIZE and the untouched valid entry still runs.
 *
 * Runs from the late acceptance hook after the current boot path (SIO
 * loaded from the recovery store through VFS, normal console bound).
 */

#include "zephyros/process.h"
#include "zos/syscall.h"
#include "service_client.h"
#include "zephyros/module.h"
#include "zephyros/kernel.h"
#include "zephyros/romfs.h"
#include "zephyros/device.h"
#include "kernel/vfs/vfs.h"

#include "process_selftest.h"

#if ZOS_SUBSYS_SELFTEST

#if ZOS_SUBSYS_SELFTEST
#include "process_expect.inc"
#endif

uint8_t process_selftest_result;

/* M9 mounts the recovery store at /SYSTEM.  The acceptance names stay
 * ROMFS-relative and are prefixed with the mountpoint here. */
static char process_path[ZOS_PATH_MAX];

static const char *process_vfs_path(const char *name)
{
	static const char prefix[] = "/SYSTEM/";
	char *out = process_path;
	const char *scan;

	for (scan = prefix; *scan != '\0'; ++scan) {
		*out++ = *scan;
	}
	for (scan = name; *scan != '\0'; ++scan) {
		*out++ = *scan;
	}
	*out = '\0';
	return process_path;
}

static int process_exec_named(const char *name, uint16_t *status)
{
	return process_exec(process_vfs_path(name), status);
}

static void process_put_hex8(uint8_t value)
{
	static const char digits[] = "0123456789ABCDEF";

	service_cli_write(digits + ((value >> 4) & 0x0Fu), 1);
	service_cli_write(digits + (value & 0x0Fu), 1);
}

static void process_put_hex16(uint16_t value)
{
	process_put_hex8((uint8_t)(value >> 8));
	process_put_hex8((uint8_t)(value & 0xFFu));
}

static void process_fail(uint8_t code, uint16_t detail)
{
	static const char prefix[] = "process selftest: FAIL 0x";

	process_selftest_result = code;
	service_cli_write(prefix, (uint16_t)(sizeof(prefix) - 1u));
	process_put_hex8(code);
	service_cli_write(" detail 0x", 10);
	process_put_hex16(detail);
	service_cli_write("\r\n", 2);
}

/* The table must return to all-FREE after every lifecycle test. */
static uint8_t process_idle_ok(void)
{
	return (uint8_t)(process_active_count() == 0 &&
			 process_free_banks() == PROCESS_BANK_COUNT &&
			 process_current_pid() == PROCESS_NO_PROCESS);
}

static void process_status_line(uint16_t status)
{
	static const char prefix[] = "process selftest: zextest status 0x";

	service_cli_write(prefix, (uint16_t)(sizeof(prefix) - 1u));
	process_put_hex16(status);
	service_cli_write("\r\n", 2);
}

/* Run one known program and require an exact exit status plus a clean
 * table.  Returns a ZOS_PCST_* code; `detail` receives the first
 * mismatch (exec result or actual status). */
static uint8_t process_run_checked(const char *name, uint16_t expected_status,
			      uint16_t *detail)
{
	uint16_t status = 0xFFFFu;
	int result = process_exec_named(name, &status);

	if (result != ZEX_EXEC_OK) {
		*detail = (uint16_t)result;
		return ZOS_PCST_FAIL_EXEC;
	}
	if (status != expected_status) {
		*detail = status;
		return ZOS_PCST_FAIL_STATUS;
	}
	if (!process_idle_ok()) {
		*detail = 0;
		return ZOS_PCST_FAIL_LEAK;
	}
	return ZOS_PCST_OK;
}

#if ZOS_SUBSYS_SELFTEST
void zos_run_process_selftest(void)
{
	uint16_t detail = 0;
	uint8_t i;
	uint8_t code;
	int result;

	process_selftest_result = ZOS_PCST_FAIL_BOOT;

	/* The ROMFS boot must have left a resident module and a bound
	 * normal console; the process table starts empty. */
	if (module_count() == 0 ||
	    module_get(0) == (const struct zmod_module *)0 ||
	    service_cli_console_handle() == 0 || !process_idle_ok()) {
		process_fail(ZOS_PCST_FAIL_BOOT, 0);
		return;
	}

	/* 1. The same ROMFS payload runs in bank 0 (lowest free bank) and
	 *    exits with PASS|0; every run leaves the table clean. */
	code = process_run_checked(ZOS_PCST_ZEXTEST_NAME,
			      (uint16_t)ZOS_PCST_ZEXTEST_PASS, &detail);
	if (code != ZOS_PCST_OK) {
		process_fail(code, detail);
		return;
	}
	{
		static const char prefix[] = "process selftest: zextest status 0x";

		service_cli_write(prefix, (uint16_t)(sizeof(prefix) - 1u));
		process_put_hex16((uint16_t)ZOS_PCST_ZEXTEST_PASS);
		service_cli_write("\r\n", 2);
	}

	/* 2. Repeated exec/exit cycles: no slot or bank leaks. */
	for (i = 0; i < 2; ++i) {
		code = process_run_checked(ZOS_PCST_ZEXTEST_NAME,
				      (uint16_t)ZOS_PCST_ZEXTEST_PASS, &detail);
		if (code != ZOS_PCST_OK) {
			process_fail(ZOS_PCST_FAIL_REPEAT, detail);
			return;
		}
	}
	{
		static const char ok[] = "process selftest: repeated cycles OK\r\n";

		service_cli_write(ok, (uint16_t)(sizeof(ok) - 1u));
	}

	/* 3. Two-bank run: reserve the allocator's lowest bank, then the
	 *    identical ZEX must run in the next bank and report it. */
	result = process_bank_reserve();
	if (result != 0) {
		process_fail(ZOS_PCST_FAIL_BANK, (uint16_t)result);
		return;
	}
	code = process_run_checked(ZOS_PCST_ZEXTEST_NAME,
			      (uint16_t)(ZOS_PCST_ZEXTEST_PASS | 1u), &detail);
	process_bank_release(0);
	if (code != ZOS_PCST_OK) {
		process_fail(ZOS_PCST_FAIL_BANK, detail);
		return;
	}
	{
		static const char second[] = "process selftest: zextest status 0x";

		service_cli_write(second, (uint16_t)(sizeof(second) - 1u));
		process_put_hex16((uint16_t)(ZOS_PCST_ZEXTEST_PASS | 1u));
		service_cli_write("\r\n", 2);
	}
	{
		static const char ok[] = "process selftest: two-bank OK\r\n";

		service_cli_write(ok, (uint16_t)(sizeof(ok) - 1u));
	}

	/* 4. Raw entry return: the common trap exits with ZOS_EFAULT. */
	code = process_run_checked(ZOS_PCST_RETURN_NAME,
			      (uint16_t)ZOS_EFAULT, &detail);
	if (code != ZOS_PCST_OK) {
		process_fail(code == ZOS_PCST_FAIL_STATUS ? ZOS_PCST_FAIL_FAULT
						     : code,
			detail);
		return;
	}
	{
		static const char ok[] = "process selftest: raw return EFAULT\r\n";

		service_cli_write(ok, (uint16_t)(sizeof(ok) - 1u));
	}

	/* 5. Every malformed fixture is rejected with its exact code and
	 *    without touching process bookkeeping. */
	for (i = 0; i < PROCESS_FIXTURE_COUNT; ++i) {
		uint16_t status = 0xFFFFu;

		result = process_exec_named(process_fixtures[i].name, &status);
		if (result != process_fixtures[i].code || !process_idle_ok()) {
			process_fail(ZOS_PCST_FAIL_REJECT,
				(uint16_t)(((uint16_t)i << 8) |
					   (uint8_t)(0 - result)));
			return;
		}
	}
	{
		static const char ok[] = "process selftest: malformed rejected\r\n";

		service_cli_write(ok, (uint16_t)(sizeof(ok) - 1u));
	}

	/* 5b. A corrupted entry in the recovery store is rejected by geometry
	 *     before any bank content changes, and the untouched valid copy
	 *     still runs afterwards: the failed load rolls back exactly. */
	{
		uint16_t status = 0;

		result = process_exec_named(ZOS_PCST_TRUNCATED_NAME, &status);
		if (result != ZEX_EXEC_ERR_FILESIZE || !process_idle_ok()) {
			process_fail(ZOS_PCST_FAIL_REJECT,
				     (uint16_t)(0 - result));
			return;
		}
	}
	code = process_run_checked(ZOS_PCST_VALID_NAME,
				   (uint16_t)ZOS_PCST_ZEXTEST_PASS, &detail);
	if (code != ZOS_PCST_OK) {
		process_fail(code, detail);
		return;
	}
	{
		static const char ok[] = "process selftest: corrupt entry rolled back\r\n";

		service_cli_write(ok, (uint16_t)(sizeof(ok) - 1u));
	}

	/* 6. Allocator exhaustion and argument validation. */
	{
		uint8_t reserved = 0;

		for (i = 0; i < PROCESS_BANK_COUNT; ++i) {
			int bank = process_bank_reserve();

			if (bank < 0 || bank > (int)PROCESS_MAX_BANK) {
				break;
			}
			++reserved;
		}
		if (reserved != PROCESS_BANK_COUNT) {
			process_fail(ZOS_PCST_FAIL_RESERVE, reserved);
			return;
		}
	}
	if (process_bank_reserve() != -(int)ZOS_ENOSPC) {
		process_fail(ZOS_PCST_FAIL_RESERVE, 0xFFFFu);
		return;
	}
	{
		uint16_t status = 0;

		result = process_exec_named(ZOS_PCST_ZEXTEST_NAME, &status);
		if (result != ZEX_EXEC_ERR_NOSPACE) {
			process_fail(ZOS_PCST_FAIL_RESERVE, (uint16_t)result);
			return;
		}
	}
	for (i = 0; i < PROCESS_BANK_COUNT; ++i) {
		process_bank_release(i);
	}
	process_bank_release(PROCESS_KERNEL_BANK); /* bank 7: must be ignored */
	if (!process_idle_ok()) {
		process_fail(ZOS_PCST_FAIL_LEAK, 7);
		return;
	}
	{
		uint16_t status = 0;

		if (process_exec((const char *)0, &status) != ZEX_EXEC_ERR_ARG ||
		    process_exec_named("absent.zex", &status) != ZEX_EXEC_ERR_ROMFS ||
		    process_exec_named(ZOS_PCST_ZEXTEST_NAME,
			    (uint16_t *)0) != ZEX_EXEC_ERR_ARG) {
			process_fail(ZOS_PCST_FAIL_ARG, (uint16_t)status);
			return;
		}
	}
	if (!process_idle_ok()) {
		process_fail(ZOS_PCST_FAIL_LEAK, 0x11);
		return;
	}

	/* 7. M8 ROMFS resource imports (driver ABI v2): name-resolved,
	 *    DATA-only, and bounded; byte-identical to the internal reader. */
	{
		romfs_file_t file;
		uint16_t rsize = 0;
		uint8_t res_buf[8];
		uint8_t ref_buf[8];
		uint16_t n;
		uint16_t j;

		if (romfs_resource_size(ZOS_PCST_DATA_NAME, &rsize) != 0 ||
		    rsize == 0) {
			process_fail(ZOS_PCST_FAIL_RESOURCE, 1); return;
		}
		if (romfs_lookup_ci(ZOS_PCST_DATA_NAME, &file) != 0 ||
		    file.flags != ROMFS_FLAG_DATA || file.size != rsize) {
			process_fail(ZOS_PCST_FAIL_RESOURCE, 2); return;
		}
		n = rsize < (uint16_t)sizeof(res_buf) ? rsize :
			(uint16_t)sizeof(res_buf);
		if (romfs_resource_read(ZOS_PCST_DATA_NAME, 0, res_buf, n) !=
				(int)n ||
		    romfs_read(&file, 0, ref_buf, n) != (int)n) {
			process_fail(ZOS_PCST_FAIL_RESOURCE, 3); return;
		}
		for (j = 0; j < n; ++j) {
			if (res_buf[j] != ref_buf[j]) {
				process_fail(ZOS_PCST_FAIL_RESOURCE, 4); return;
			}
		}
		/* Exact tail read succeeds; one byte past EOF is rejected. */
		if (romfs_resource_read(ZOS_PCST_DATA_NAME,
					(uint16_t)(rsize - n), res_buf, n) !=
				(int)n ||
		    romfs_resource_read(ZOS_PCST_DATA_NAME, rsize, res_buf, 1) !=
				ZDEV_EINVAL) {
			process_fail(ZOS_PCST_FAIL_RESOURCE, 5); return;
		}
		/* ZMOD/ZEX files are not importable; missing names and null
		 * buffers fail predictably. */
		if (romfs_resource_size(ZOS_PCST_ZEXTEST_NAME, &rsize) !=
				ZDEV_EINVAL ||
		    romfs_resource_size("sio.zmod", &rsize) != ZDEV_EINVAL ||
		    romfs_resource_size("absent.bin", &rsize) != ZDEV_ENOENT ||
		    romfs_resource_read(ZOS_PCST_DATA_NAME, 0, (void *)0, 1) !=
				ZDEV_EINVAL) {
			process_fail(ZOS_PCST_FAIL_RESOURCE, 6); return;
		}
	}

	process_selftest_result = ZOS_PCST_OK;
	{
		static const char ok[] = "process selftest: OK\r\n";

		if (service_cli_write(ok, (uint16_t)(sizeof(ok) - 1u)) !=
		    (int)(sizeof(ok) - 1u)) {
			process_selftest_result = ZOS_PCST_FAIL_CONSOLE;
		}
	}
}
#endif /* ZOS_SUBSYS_SELFTEST */


#endif /* ZOS_SUBSYS_SELFTEST */
