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
 *   2. exec the recursive `fsdeep.zex` depth probe: its exit status is
 *      exactly ZOS_EXEC_MAX_DEPTH, so the level past the limit was refused
 *      with ZOS_EBUSY.
 *   3. drive the two section-33 error paths that cannot be produced
 *      deterministically on demand -- a stale backend handle and a failed
 *      confirmed write.  The simulator arms the fault on the `arm` fence;
 *      on a real card the operator's card change is the trigger and the
 *      matching `*:fresh` note is recorded instead.
 *   4. check the descriptor pool, the process table and the bank
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

#if ZOS_M9_SELFTEST

uint8_t m9_integration_selftest_result;

#define M9S_ROOT "/ZOSTEST"
#define M9S_ST   "/ZOSTEST/FT.TXT"
#define M9S_SIZE 1040u

static uint8_t m9s_buf[520];
static uint8_t m9s_ref[512];
static struct vfs_file m9s_file;
static struct zfs_stat m9s_stat;

static void m9s_puts(const char *s)
{
	early_console_puts(s);
}

static void m9s_hex8(uint8_t value)
{
	static const char hex[] = "0123456789ABCDEF";

	early_console_putc(hex[(value >> 4) & 0x0Fu]);
	early_console_putc(hex[value & 0x0Fu]);
}

static void m9s_fail(uint8_t code)
{
	m9_integration_selftest_result = code;
	m9s_puts("m9 selftest: FAIL 0x");
	m9s_hex8(code);
	m9s_puts("\r\n");
}

static void m9s_note(const char *s)
{
	m9s_puts("m9 selftest: ");
	m9s_puts(s);
	m9s_puts("\r\n");
}

static void m9s_pattern(void)
{
	uint16_t i;

	for (i = 0; i < 512u; ++i) {
		m9s_ref[i] = (uint8_t)((uint16_t)i * 29u + 11u);
	}
}

static uint8_t m9s_slice_diff(const uint8_t *buf, uint16_t offset,
			      uint16_t len)
{
	uint16_t i;

	for (i = 0; i < len; ++i) {
		if (buf[i] != m9s_ref[(uint16_t)(offset + i) & 511u]) {
			return 1u;
		}
	}
	return 0u;
}

/* 1 = `/` mounted, 0 = the documented absent-media path, -1 = anything
 * else. */
static int m9s_root_mounted(void)
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

/* Write a 512-periodic file through create+truncate. */
static int m9s_write_file(const char *path, uint16_t len)
{
	uint16_t done = 0u;

	if (vfs_open(path, (uint16_t)(ZFS_O_WRITE | ZFS_O_CREATE |
				      ZFS_O_TRUNC), &m9s_file) != 0) {
		return -1;
	}
	while (done < len) {
		uint16_t chunk = (uint16_t)(len - done);

		if (chunk > 512u) {
			chunk = 512u;
		}
		if (vfs_write(&m9s_file, m9s_ref, chunk) != 0) {
			(void)vfs_close(&m9s_file);
			return -1;
		}
		done = (uint16_t)(done + chunk);
	}
	return vfs_close(&m9s_file);
}

/* The stale-handle and failed-confirmed-write probes.  Each window has an
 * `arm` fence the simulator keys its fault injection on, an outcome note
 * (`observed` when the fault reached the provider, `fresh` on real media
 * with no fault injected) and an `end` fence. */
static int m9s_faults(void)
{
	uint32_t pos = 0u;
	int result;

	/* fstest and its deep-matrix child both create and remove `/ZOSTEST`,
	 * so the fault window owns the directory it uses. */
	result = vfs_mkdir(M9S_ROOT);
	if (result != 0 && result != -ZOS_EEXIST) {
		return 1;
	}
	if (m9s_write_file(M9S_ST, M9S_SIZE) != 0) {
		return 2;
	}

	/* A stale backend handle: the FS2 generation changes while the file
	 * is open.  Block 1 is not resident, so the read reaches the lane. */
	if (vfs_open(M9S_ST, ZFS_O_READ, &m9s_file) != 0) {
		return 3;
	}
	if (vfs_read(&m9s_file, m9s_buf, 16u) != 16 ||
	    m9s_slice_diff(m9s_buf, 0u, 16u) != 0u) {
		(void)vfs_close(&m9s_file);
		return 3;
	}
	m9s_note("fault:stale:arm");
	if (vfs_seek(&m9s_file, 1024, ZFS_SEEK_SET, &pos) != 0) {
		(void)vfs_close(&m9s_file);
		return 4;
	}
	result = vfs_read(&m9s_file, m9s_buf, 16u);
	if (result == -ZOS_ESTALE) {
		m9s_note("fault:stale:observed");
	} else if (result == 16 &&
		   m9s_slice_diff(m9s_buf, 1024u, 16u) == 0u) {
		m9s_note("fault:stale:fresh");
	} else {
		(void)vfs_close(&m9s_file);
		return 4;
	}
	m9s_note("fault:stale:end");
	(void)vfs_close(&m9s_file);

	/* A failed confirmed write: the write command is accepted but the
	 * DONE confirmation fails, so the provider reports EIO and neither
	 * the offset nor the size may move. */
	if (vfs_open(M9S_ST, ZFS_O_WRITE, &m9s_file) != 0) {
		return 5;
	}
	if (vfs_seek(&m9s_file, 4, ZFS_SEEK_SET, &pos) != 0 || pos != 4u) {
		(void)vfs_close(&m9s_file);
		return 5;
	}
	m9s_note("fault:write:arm");
	result = vfs_write(&m9s_file, m9s_ref, 4u);
	if (result == -ZOS_EIO) {
		m9s_note("fault:write:observed");
		if (vfs_seek(&m9s_file, 0, ZFS_SEEK_CUR, &pos) != 0 ||
		    pos != 4u) {
			(void)vfs_close(&m9s_file);
			return 6;
		}
		if (vfs_stat(M9S_ST, &m9s_stat) != 0 ||
		    m9s_stat.size != (uint32_t)M9S_SIZE) {
			(void)vfs_close(&m9s_file);
			return 6;
		}
	} else if (result == 0) {
		m9s_note("fault:write:fresh");
	} else {
		(void)vfs_close(&m9s_file);
		return 6;
	}
	m9s_note("fault:write:end");
	(void)vfs_close(&m9s_file);

	if (vfs_unlink(M9S_ST) != 0) {
		return 7;
	}
	return 0;
}

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

	m9s_pattern();

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

	result = m9s_faults();
	if (result != 0) {
		/* Name the window step so the transcript is actionable. */
		m9s_puts("m9 selftest: fault window step 0x");
		m9s_hex8((uint8_t)result);
		m9s_puts("\r\n");
		m9s_fail(ZOS_M9IST_FAIL_FAULT);
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

#endif /* ZOS_M9_SELFTEST */
