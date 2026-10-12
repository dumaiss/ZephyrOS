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

#include "process.h"
#include "zos/syscall.h"
#include "driver_client.h"
#include "kernel.h"
#include "romfs.h"
#include "zos/errno.h"
#include "kernel/vfs/vfs.h"

#include "process_selftest.h"

#if ZOS_SUBSYS_SELFTEST

#if ZOS_SUBSYS_SELFTEST
#include "process_expect.inc"
#endif

uint8_t process_selftest_result;

/* M9 mounts the recovery store at /system.  The acceptance names stay
 * ROMFS-relative and are prefixed with the mountpoint here. */
static char process_path[ZOS_PATH_MAX];

/* --- ZEX v2 loader-outcome helpers (work package 44a) -----------------
 *
 * The loader-outcome tests write into a free application bank through
 * the common copy gate before an exec, so the child can prove from
 * inside what the v2 loader did or did not clear (36.19).  The common
 * staging block is one 64-byte chunk; longer runs are chunked here. */
#define PROCESS_Z2_CHUNK 64u

static void zex2_bank_fill(uint8_t bank, uint16_t addr, uint16_t length,
			   uint8_t value)
{
	struct zos_bank_copy_req req;
	uint16_t done = 0u;
	uint16_t i;

	for (i = 0u; i < PROCESS_Z2_CHUNK; ++i) {
		zos_user_copy_staging[i] = value;
	}
	while (done < length) {
		uint16_t chunk = (uint16_t)(length - done);

		if (chunk > PROCESS_Z2_CHUNK) {
			chunk = PROCESS_Z2_CHUNK;
		}
		req.bank = bank;
		req.op = ZOS_BANK_OP_WRITE;
		req.addr = (uint16_t)(addr + done);
		req.buffer = zos_user_copy_staging;
		req.length = chunk;
		(void)zos_bank_copy(&req);
		done = (uint16_t)(done + chunk);
	}
}

/* The marker the probe programs must still find outside image/BSS/stack. */
static void zex2_marker_write(uint8_t bank)
{
	struct zos_bank_copy_req req;

	zos_user_copy_staging[0] = ZOS_PCST_ZEX2_MARK_0;
	zos_user_copy_staging[1] = ZOS_PCST_ZEX2_MARK_1;
	zos_user_copy_staging[2] = ZOS_PCST_ZEX2_MARK_2;
	zos_user_copy_staging[3] = ZOS_PCST_ZEX2_MARK_3;
	req.bank = bank;
	req.op = ZOS_BANK_OP_WRITE;
	req.addr = (uint16_t)ZOS_PCST_ZEX2_MARK_ADDR;
	req.buffer = zos_user_copy_staging;
	req.length = (uint16_t)ZOS_PCST_ZEX2_MARK_SIZE;
	(void)zos_bank_copy(&req);
}

static uint8_t zex2_marker_check(uint8_t bank)
{
	struct zos_bank_copy_req req;

	req.bank = bank;
	req.op = ZOS_BANK_OP_READ;
	req.addr = (uint16_t)ZOS_PCST_ZEX2_MARK_ADDR;
	req.buffer = zos_user_copy_staging;
	req.length = (uint16_t)ZOS_PCST_ZEX2_MARK_SIZE;
	if (zos_bank_copy(&req) != 0u) {
		return 0u;
	}
	return (uint8_t)(zos_user_copy_staging[0] == ZOS_PCST_ZEX2_MARK_0 &&
			 zos_user_copy_staging[1] == ZOS_PCST_ZEX2_MARK_1 &&
			 zos_user_copy_staging[2] == ZOS_PCST_ZEX2_MARK_2 &&
			 zos_user_copy_staging[3] == ZOS_PCST_ZEX2_MARK_3);
}

static const char *process_vfs_path(const char *name)
{
	static const char prefix[] = "/system/";
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

	driver_cli_write(digits + ((value >> 4) & 0x0Fu), 1);
	driver_cli_write(digits + (value & 0x0Fu), 1);
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
	driver_cli_write(prefix, (uint16_t)(sizeof(prefix) - 1u));
	process_put_hex8(code);
	driver_cli_write(" detail 0x", 10);
	process_put_hex16(detail);
	driver_cli_write("\r\n", 2);
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

	driver_cli_write(prefix, (uint16_t)(sizeof(prefix) - 1u));
	process_put_hex16(status);
	driver_cli_write("\r\n", 2);
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

	/* The resident driver boot must have left a bound normal
	 * console; the process table starts empty. */
	if (driver_cli_console_handle() == 0 || !process_idle_ok()) {
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

		driver_cli_write(prefix, (uint16_t)(sizeof(prefix) - 1u));
		process_put_hex16((uint16_t)ZOS_PCST_ZEXTEST_PASS);
		driver_cli_write("\r\n", 2);
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

		driver_cli_write(ok, (uint16_t)(sizeof(ok) - 1u));
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

		driver_cli_write(second, (uint16_t)(sizeof(second) - 1u));
		process_put_hex16((uint16_t)(ZOS_PCST_ZEXTEST_PASS | 1u));
		driver_cli_write("\r\n", 2);
	}
	{
		static const char ok[] = "process selftest: two-bank OK\r\n";

		driver_cli_write(ok, (uint16_t)(sizeof(ok) - 1u));
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

		driver_cli_write(ok, (uint16_t)(sizeof(ok) - 1u));
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

		driver_cli_write(ok, (uint16_t)(sizeof(ok) - 1u));
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

		driver_cli_write(ok, (uint16_t)(sizeof(ok) - 1u));
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
				(-ZOS_EINVAL)) {
			process_fail(ZOS_PCST_FAIL_RESOURCE, 5); return;
		}
		/* ZMOD/ZEX files are not importable; missing names and null
		 * buffers fail predictably. */
		if (romfs_resource_size(ZOS_PCST_ZEXTEST_NAME, &rsize) !=
				(-ZOS_EINVAL) ||
		    romfs_resource_size("sio.zmod", &rsize) != (-ZOS_EINVAL) ||
		    romfs_resource_size("absent.bin", &rsize) != (-ZOS_ENOENT) ||
		    romfs_resource_read(ZOS_PCST_DATA_NAME, 0, (void *)0, 1) !=
				(-ZOS_EINVAL)) {
			process_fail(ZOS_PCST_FAIL_RESOURCE, 6); return;
		}
	}

	/* 8. ZEX v2 loader outcomes (work package 44a).  The v2 loader
	 *    zeroes only BSS and the startup block (36.19), places DATA
	 *    segments as process-owned extended-memory objects and handles
	 *    a container over 64 KiB.  The kernel side dirties the child's
	 *    bank through the copy gate first, so the values the programs
	 *    read back prove exactly what the loader did. */
	{
		/* 8a. BSS zeroed, bank not cleared: dirty the probe's whole
		 * BSS range and write the marker outside its
		 * image/BSS/stack in bank 0 - the lowest free bank the
		 * child will get.  The probe checks both from inside and
		 * the marker is checked here again after it exited. */
		zex2_bank_fill(0u, (uint16_t)ZEX2_PROBE_BSS_START,
			       (uint16_t)(ZEX2_PROBE_BSS_END -
					  ZEX2_PROBE_BSS_START),
			       ZOS_PCST_ZEX2_MARK_0);
		zex2_marker_write(0u);
		code = process_run_checked(ZOS_PCST_PROBE_NAME,
					   (uint16_t)ZOS_PCST_ZEX2_PASS,
					   &detail);
		if (code != ZOS_PCST_OK) {
			process_fail(ZOS_PCST_FAIL_ZEX2_BSS, detail);
			return;
		}
		if (!zex2_marker_check(0u)) {
			process_fail(ZOS_PCST_FAIL_ZEX2_BSS, 0x5A5Au);
			return;
		}
		{
			static const char ok[] =
				"process selftest: zex2 BSS/bank OK\r\n";

			driver_cli_write(ok, (uint16_t)(sizeof(ok) - 1u));
		}

		/* 8b. DATA segment: dirty the auxiliary bank's segment-BSS
		 * area (the clean bank map places the first one-slot object
		 * in bank 5, slot 3, at 6000h), then run the segment
		 * program; it reads the dirty area back as zero. */
		zex2_bank_fill(ZOS_PCST_SEG_BANK,
			       (uint16_t)(ZOS_PCST_SEG_BASE +
					  ZEX2_SEG_SEG1_IMAGE),
			       (uint16_t)ZEX2_SEG_SEG1_BSS,
			       ZOS_PCST_ZEX2_MARK_0);
		code = process_run_checked(ZOS_PCST_SEG_NAME,
					   (uint16_t)ZOS_PCST_ZEX2_PASS,
					   &detail);
		if (code != ZOS_PCST_OK) {
			process_fail(ZOS_PCST_FAIL_ZEX2_SEG, detail);
			return;
		}
		{
			static const char ok[] =
				"process selftest: zex2 segment OK\r\n";

			driver_cli_write(ok, (uint16_t)(sizeof(ok) - 1u));
		}

		/* 8c. A container over 64 KiB: two 32-KiB DATA segments,
		 * the second one's last byte past the 64-KiB file offset. */
		code = process_run_checked(ZOS_PCST_BIG_NAME,
					   (uint16_t)ZOS_PCST_ZEX2_PASS,
					   &detail);
		if (code != ZOS_PCST_OK) {
			process_fail(ZOS_PCST_FAIL_ZEX2_BIG, detail);
			return;
		}
		{
			static const char ok[] =
				"process selftest: zex2 big container OK\r\n";

			driver_cli_write(ok, (uint16_t)(sizeof(ok) - 1u));
		}

		/* 8d. Segment-allocation rollback: with banks 0-4 reserved
		 * the child would take bank 5, and its DATA segment has no
		 * auxiliary bank at all, so the load must fail with NOSPACE
		 * and leave no slot, bank, object or segment behind. */
		for (i = 0u; i < (PROCESS_BANK_COUNT - 1u); ++i) {
			int bank = process_bank_reserve();

			if (bank != (int)i) {
				process_fail(ZOS_PCST_FAIL_ZEX2_ROLL,
					     (uint16_t)bank);
				return;
			}
		}
		{
			uint16_t status = 0xFFFFu;

			result = process_exec_named(ZOS_PCST_SEG_NAME,
						    &status);
			if (result != ZEX_EXEC_ERR_NOSPACE ||
			    process_active_count() != 0u ||
			    process_current_pid() != PROCESS_NO_PROCESS) {
				process_fail(ZOS_PCST_FAIL_ZEX2_ROLL,
					     (uint16_t)(0 - result));
				return;
			}
		}
		for (i = 0u; i < (PROCESS_BANK_COUNT - 1u); ++i) {
			process_bank_release(i);
		}
		if (!process_idle_ok()) {
			process_fail(ZOS_PCST_FAIL_ZEX2_ROLL, 0xFFFFu);
			return;
		}
		{
			static const char ok[] =
				"process selftest: zex2 rollback OK\r\n";

			driver_cli_write(ok, (uint16_t)(sizeof(ok) - 1u));
		}

		/* 8e. The rejected program still runs once the release
		 * restored the allocator, with its segment BSS dirtied
		 * again. */
		zex2_bank_fill(ZOS_PCST_SEG_BANK,
			       (uint16_t)(ZOS_PCST_SEG_BASE +
					  ZEX2_SEG_SEG1_IMAGE),
			       (uint16_t)ZEX2_SEG_SEG1_BSS,
			       ZOS_PCST_ZEX2_MARK_0);
		code = process_run_checked(ZOS_PCST_SEG_NAME,
					   (uint16_t)ZOS_PCST_ZEX2_PASS,
					   &detail);
		if (code != ZOS_PCST_OK) {
			process_fail(ZOS_PCST_FAIL_ZEX2_SEG, detail);
			return;
		}
		{
			static const char ok[] =
				"process selftest: zex v2 outcomes OK\r\n";

			driver_cli_write(ok, (uint16_t)(sizeof(ok) - 1u));
		}
	}

	process_selftest_result = ZOS_PCST_OK;
	{
		static const char ok[] = "process selftest: OK\r\n";

		if (driver_cli_write(ok, (uint16_t)(sizeof(ok) - 1u)) !=
		    (int)(sizeof(ok) - 1u)) {
			process_selftest_result = ZOS_PCST_FAIL_CONSOLE;
		}
	}
}
#endif /* ZOS_SUBSYS_SELFTEST */


#endif /* ZOS_SUBSYS_SELFTEST */
