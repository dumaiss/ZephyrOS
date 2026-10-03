/* ZephyrOS M6 process/ZEX acceptance selftests (test artifact).
 *
 * Two functions, selected by the build:
 *
 *   zos_run_m6_selftest()          success image (ZOS_M6_SELFTEST)
 *     - zextest runs from ROMFS in bank 0 and exits with PASS|0;
 *     - repeated exec/exit cycles leak no slot or bank;
 *     - reserving bank 0 through the tested allocator makes the same
 *       ROMFS payload run in bank 1 (PASS|1);
 *     - a raw entry return exits with ZOS_EFAULT through the common
 *       trap;
 *     - every malformed fixture is rejected with its exact code and
 *       leaves the table clean;
 *     - argument errors and allocator exhaustion are precise;
 *     - bank 7 stays unreachable through the normal process API.
 *
 *   zos_run_m6_failure_selftest()  failure image (ZOS_M6_FAILURE_SELFTEST)
 *     - the ROMFS "zextest.zex" entry is a truncated ZEX: the load must
 *       fail with ZEX_EXEC_ERR_FILESIZE and roll back exactly;
 *     - the valid copy packed as "zexok.zex" must then run normally.
 *
 * Runs from the late acceptance hook after the production-style boot
 * (SIO loaded from ROMFS, normal console bound).
 */

#include "zephyros/process.h"
#include "zephyros/syscall.h"
#include "zephyros/console.h"
#include "zephyros/module.h"
#include "zephyros/kernel.h"
#include "zephyros/romfs.h"
#include "zephyros/device.h"

#include "m6_selftest.h"

#if ZOS_M6_SELFTEST || ZOS_M6_FAILURE_SELFTEST

#if ZOS_M6_SELFTEST
#include "m6_expect.inc"
#endif

uint8_t m6_selftest_result;
static void m6_put_hex8(uint8_t value)
{
	static const char digits[] = "0123456789ABCDEF";

	console_write(digits + ((value >> 4) & 0x0Fu), 1);
	console_write(digits + (value & 0x0Fu), 1);
}

static void m6_put_hex16(uint16_t value)
{
	m6_put_hex8((uint8_t)(value >> 8));
	m6_put_hex8((uint8_t)(value & 0xFFu));
}

static void m6_fail(uint8_t code, uint16_t detail)
{
	static const char prefix[] = "m6 selftest: FAIL 0x";

	m6_selftest_result = code;
	console_write(prefix, (uint16_t)(sizeof(prefix) - 1u));
	m6_put_hex8(code);
	console_write(" detail 0x", 10);
	m6_put_hex16(detail);
	console_write("\r\n", 2);
}

static void m6f_fail(uint8_t code, uint16_t detail)
{
	static const char prefix[] = "m6 failure selftest: FAIL 0x";

	m6_selftest_result = code;
	console_write(prefix, (uint16_t)(sizeof(prefix) - 1u));
	m6_put_hex8(code);
	console_write(" detail 0x", 10);
	m6_put_hex16(detail);
	console_write("\r\n", 2);
}

/* The table must return to all-FREE after every lifecycle test. */
static uint8_t m6_idle_ok(void)
{
	return (uint8_t)(process_active_count() == 0 &&
			 process_free_banks() == PROCESS_BANK_COUNT &&
			 process_current_pid() == PROCESS_NO_PROCESS);
}

static void m6_status_line(uint16_t status)
{
	static const char prefix[] = "m6 selftest: zextest status 0x";

	console_write(prefix, (uint16_t)(sizeof(prefix) - 1u));
	m6_put_hex16(status);
	console_write("\r\n", 2);
}

/* Run one known program and require an exact exit status plus a clean
 * table.  Returns a ZOS_M6ST_* code; `detail` receives the first
 * mismatch (exec result or actual status). */
static uint8_t m6_run_checked(const char *name, uint16_t expected_status,
			      uint16_t *detail)
{
	uint16_t status = 0xFFFFu;
	int result = process_exec(name, &status);

	if (result != ZEX_EXEC_OK) {
		*detail = (uint16_t)result;
		return ZOS_M6ST_FAIL_EXEC;
	}
	if (status != expected_status) {
		*detail = status;
		return ZOS_M6ST_FAIL_STATUS;
	}
	if (!m6_idle_ok()) {
		*detail = 0;
		return ZOS_M6ST_FAIL_LEAK;
	}
	return ZOS_M6ST_OK;
}

#if ZOS_M6_SELFTEST
void zos_run_m6_selftest(void)
{
	uint16_t detail = 0;
	uint8_t i;
	uint8_t code;
	int result;

	m6_selftest_result = ZOS_M6ST_FAIL_BOOT;

	/* The ROMFS boot must have left a resident module and a bound
	 * normal console; the process table starts empty. */
	if (module_count() == 0 ||
	    module_get(0) == (const struct zmod_module *)0 ||
	    console_output_handle() == 0 || !m6_idle_ok()) {
		m6_fail(ZOS_M6ST_FAIL_BOOT, 0);
		return;
	}

	/* 1. The same ROMFS payload runs in bank 0 (lowest free bank) and
	 *    exits with PASS|0; every run leaves the table clean. */
	code = m6_run_checked(ZOS_M6ST_ZEXTEST_NAME,
			      (uint16_t)ZOS_M6ST_ZEXTEST_PASS, &detail);
	if (code != ZOS_M6ST_OK) {
		m6_fail(code, detail);
		return;
	}
	{
		static const char prefix[] = "m6 selftest: zextest status 0x";

		console_write(prefix, (uint16_t)(sizeof(prefix) - 1u));
		m6_put_hex16((uint16_t)ZOS_M6ST_ZEXTEST_PASS);
		console_write("\r\n", 2);
	}

	/* 2. Repeated exec/exit cycles: no slot or bank leaks. */
	for (i = 0; i < 2; ++i) {
		code = m6_run_checked(ZOS_M6ST_ZEXTEST_NAME,
				      (uint16_t)ZOS_M6ST_ZEXTEST_PASS, &detail);
		if (code != ZOS_M6ST_OK) {
			m6_fail(ZOS_M6ST_FAIL_REPEAT, detail);
			return;
		}
	}
	{
		static const char ok[] = "m6 selftest: repeated cycles OK\r\n";

		console_write(ok, (uint16_t)(sizeof(ok) - 1u));
	}

	/* 3. Two-bank run: reserve the allocator's lowest bank, then the
	 *    identical ZEX must run in the next bank and report it. */
	result = process_bank_reserve();
	if (result != 0) {
		m6_fail(ZOS_M6ST_FAIL_BANK, (uint16_t)result);
		return;
	}
	code = m6_run_checked(ZOS_M6ST_ZEXTEST_NAME,
			      (uint16_t)(ZOS_M6ST_ZEXTEST_PASS | 1u), &detail);
	process_bank_release(0);
	if (code != ZOS_M6ST_OK) {
		m6_fail(ZOS_M6ST_FAIL_BANK, detail);
		return;
	}
	{
		static const char second[] = "m6 selftest: zextest status 0x";

		console_write(second, (uint16_t)(sizeof(second) - 1u));
		m6_put_hex16((uint16_t)(ZOS_M6ST_ZEXTEST_PASS | 1u));
		console_write("\r\n", 2);
	}
	{
		static const char ok[] = "m6 selftest: two-bank OK\r\n";

		console_write(ok, (uint16_t)(sizeof(ok) - 1u));
	}

	/* 4. Raw entry return: the common trap exits with ZOS_EFAULT. */
	code = m6_run_checked(ZOS_M6ST_RETURN_NAME,
			      (uint16_t)ZOS_EFAULT, &detail);
	if (code != ZOS_M6ST_OK) {
		m6_fail(code == ZOS_M6ST_FAIL_STATUS ? ZOS_M6ST_FAIL_FAULT
						     : code,
			detail);
		return;
	}
	{
		static const char ok[] = "m6 selftest: raw return EFAULT\r\n";

		console_write(ok, (uint16_t)(sizeof(ok) - 1u));
	}

	/* 5. Every malformed fixture is rejected with its exact code and
	 *    without touching process bookkeeping. */
	for (i = 0; i < M6_FIXTURE_COUNT; ++i) {
		uint16_t status = 0xFFFFu;

		result = process_exec(m6_fixtures[i].name, &status);
		if (result != m6_fixtures[i].code || !m6_idle_ok()) {
			m6_fail(ZOS_M6ST_FAIL_REJECT,
				(uint16_t)(((uint16_t)i << 8) |
					   (uint8_t)(0 - result)));
			return;
		}
	}
	{
		static const char ok[] = "m6 selftest: malformed rejected\r\n";

		console_write(ok, (uint16_t)(sizeof(ok) - 1u));
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
			m6_fail(ZOS_M6ST_FAIL_RESERVE, reserved);
			return;
		}
	}
	if (process_bank_reserve() != -(int)ZOS_ENOSPC) {
		m6_fail(ZOS_M6ST_FAIL_RESERVE, 0xFFFFu);
		return;
	}
	{
		uint16_t status = 0;

		result = process_exec(ZOS_M6ST_ZEXTEST_NAME, &status);
		if (result != ZEX_EXEC_ERR_NOSPACE) {
			m6_fail(ZOS_M6ST_FAIL_RESERVE, (uint16_t)result);
			return;
		}
	}
	for (i = 0; i < PROCESS_BANK_COUNT; ++i) {
		process_bank_release(i);
	}
	process_bank_release(PROCESS_KERNEL_BANK); /* bank 7: must be ignored */
	if (!m6_idle_ok()) {
		m6_fail(ZOS_M6ST_FAIL_LEAK, 7);
		return;
	}
	{
		uint16_t status = 0;

		if (process_exec((const char *)0, &status) != ZEX_EXEC_ERR_ARG ||
		    process_exec("absent.zex", &status) != ZEX_EXEC_ERR_ROMFS ||
		    process_exec(ZOS_M6ST_ZEXTEST_NAME,
				(uint16_t *)0) != ZEX_EXEC_ERR_ARG) {
			m6_fail(ZOS_M6ST_FAIL_ARG, (uint16_t)status);
			return;
		}
	}
	if (!m6_idle_ok()) {
		m6_fail(ZOS_M6ST_FAIL_LEAK, 0x11);
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

		if (romfs_resource_size(ZOS_M6ST_DATA_NAME, &rsize) != 0 ||
		    rsize == 0) {
			m6_fail(ZOS_M6ST_FAIL_RESOURCE, 1); return;
		}
		if (romfs_lookup(ZOS_M6ST_DATA_NAME, &file) != 0 ||
		    file.flags != ROMFS_FLAG_DATA || file.size != rsize) {
			m6_fail(ZOS_M6ST_FAIL_RESOURCE, 2); return;
		}
		n = rsize < (uint16_t)sizeof(res_buf) ? rsize :
			(uint16_t)sizeof(res_buf);
		if (romfs_resource_read(ZOS_M6ST_DATA_NAME, 0, res_buf, n) !=
				(int)n ||
		    romfs_read(&file, 0, ref_buf, n) != (int)n) {
			m6_fail(ZOS_M6ST_FAIL_RESOURCE, 3); return;
		}
		for (j = 0; j < n; ++j) {
			if (res_buf[j] != ref_buf[j]) {
				m6_fail(ZOS_M6ST_FAIL_RESOURCE, 4); return;
			}
		}
		/* Exact tail read succeeds; one byte past EOF is rejected. */
		if (romfs_resource_read(ZOS_M6ST_DATA_NAME,
					(uint16_t)(rsize - n), res_buf, n) !=
				(int)n ||
		    romfs_resource_read(ZOS_M6ST_DATA_NAME, rsize, res_buf, 1) !=
				ZDEV_EINVAL) {
			m6_fail(ZOS_M6ST_FAIL_RESOURCE, 5); return;
		}
		/* ZMOD/ZEX files are not importable; missing names and null
		 * buffers fail predictably. */
		if (romfs_resource_size(ZOS_M6ST_ZEXTEST_NAME, &rsize) !=
				ZDEV_EINVAL ||
		    romfs_resource_size("sio.zmod", &rsize) != ZDEV_EINVAL ||
		    romfs_resource_size("absent.bin", &rsize) != ZDEV_ENOENT ||
		    romfs_resource_read(ZOS_M6ST_DATA_NAME, 0, (void *)0, 1) !=
				ZDEV_EINVAL) {
			m6_fail(ZOS_M6ST_FAIL_RESOURCE, 6); return;
		}
	}

	m6_selftest_result = ZOS_M6ST_OK;
	{
		static const char ok[] = "m6 selftest: OK\r\n";

		if (console_write(ok, (uint16_t)(sizeof(ok) - 1u)) !=
		    (int)(sizeof(ok) - 1u)) {
			m6_selftest_result = ZOS_M6ST_FAIL_CONSOLE;
		}
	}
}
#endif /* ZOS_M6_SELFTEST */

#if ZOS_M6_FAILURE_SELFTEST
void zos_run_m6_failure_selftest(void)
{
	uint16_t status = 0;
	int result;

	m6_selftest_result = ZOS_M6ST_FAIL_BOOT;
	if (module_count() == 0 || console_output_handle() == 0 ||
	    !m6_idle_ok()) {
		m6f_fail(ZOS_M6ST_FAIL_BOOT, 0);
		return;
	}

	/* The corrupted payload must be rejected by geometry, before any
	 * bank content changes, and the reservation must roll back. */
	result = process_exec(ZOS_M6ST_FAILURE_NAME, &status);
	if (result != ZEX_EXEC_ERR_FILESIZE) {
		m6f_fail(ZOS_M6ST_FAIL_EXEC, (uint16_t)result);
		return;
	}
	if (!m6_idle_ok()) {
		m6f_fail(ZOS_M6ST_FAIL_LEAK, 0);
		return;
	}

	/* The untouched valid copy must still load and run: the failed
	 * load left the allocator and the bank in a clean state. */
	result = process_exec(ZOS_M6ST_FAILURE_OK_NAME, &status);
	if (result != ZEX_EXEC_OK) {
		m6f_fail(ZOS_M6ST_FAIL_EXEC, (uint16_t)result);
		return;
	}
	if (status != (uint16_t)ZOS_M6ST_ZEXTEST_PASS) {
		m6f_fail(ZOS_M6ST_FAIL_STATUS, status);
		return;
	}
	if (!m6_idle_ok()) {
		m6f_fail(ZOS_M6ST_FAIL_LEAK, 0x22);
		return;
	}

	m6_selftest_result = ZOS_M6ST_OK;
	{
		static const char ok[] = "m6 failure selftest: OK\r\n";

		if (console_write(ok, (uint16_t)(sizeof(ok) - 1u)) !=
		    (int)(sizeof(ok) - 1u)) {
			m6_selftest_result = ZOS_M6ST_FAIL_CONSOLE;
		}
	}
}
#endif /* ZOS_M6_FAILURE_SELFTEST */

#endif /* ZOS_M6_SELFTEST || ZOS_M6_FAILURE_SELFTEST */
