/* ZephyrOS M9 writable-provider acceptance (target, test only).
 *
 * Runs in the SIO-only m9-provider-selftest image after boot.  The test-only
 * ramfs ZMOD has registered through driver ABI v3 and the normal mount pass
 * attached it at `/` (required) and `/R2` (optional); the fixture modules
 * `m9.probe`, `m9.dup` and `m9.fail` exercised provider registration,
 * duplicate rejection and load rollback during the module phase.
 *
 * Coverage: provider registry/rollback, mount state and the refuse-mount
 * control, open-flag and EISDIR/ENOTDIR/EEXIST errors, mkdir/rmdir/unlink,
 * rename and EXDEV, stat, the root merge with mountpoint shadowing, the
 * section-33 size set, 32-bit seek (including past 64 KiB), stale handles,
 * a failed confirmed write leaving the offset and size unchanged, and the
 * provider's per-operation counters.
 *
 * Diagnostics go over the direct early console so the transcript does not
 * depend on which console the boot bound.
 */

#include "zephyros/fs.h"
#include "zephyros/vfs.h"
#include "zephyros/module.h"
#include "zephyros/console.h"
#include "zephyros/kernel.h"

#include "kernel/vfs/vfs.h"

#include "m9_provider_selftest.h"

#if ZOS_M9_PROVIDER_SELFTEST

uint8_t m9_provider_selftest_result;

static char m9p_path[ZOS_PATH_MAX];
static char m9p_path2[ZOS_PATH_MAX];
static uint8_t m9p_buf[520];
static uint8_t m9p_ref[520];
static struct zfs_dirent m9p_dirent;
static struct zfs_stat m9p_stat;
static struct m9p_stats m9p_stats;
static struct vfs_file m9p_file;

static void m9p_put_hex8(uint8_t value)
{
	static const char hex[] = "0123456789ABCDEF";

	early_console_putc(hex[(value >> 4) & 0x0Fu]);
	early_console_putc(hex[value & 0x0Fu]);
}

static void m9p_fail(uint8_t code)
{
	m9_provider_selftest_result = code;
	early_console_puts("m9 provider selftest: FAIL 0x");
	m9p_put_hex8(code);
	early_console_puts("\r\n");
}

static void m9p_copy(char *dst, const char *src)
{
	uint16_t i = 0;

	while (src[i] != '\0' && i + 1u < ZOS_PATH_MAX) {
		dst[i] = src[i];
		++i;
	}
	dst[i] = '\0';
}

static const char *m9p_set(char *dst, const char *text)
{
	m9p_copy(dst, text);
	return dst;
}

static void m9p_pattern(uint8_t *buf, uint16_t len)
{
	uint16_t i;

	for (i = 0; i < len; ++i) {
		buf[i] = (uint8_t)((uint16_t)i * 31u + 7u);
	}
}

static int m9p_mount_index(const char *path)
{
	uint16_t i;

	for (i = 0; i < vfs_mount_count; ++i) {
		if (vfs_mount_state[i].state == VFS_MOUNT_MOUNTED &&
		    vfs_name_equal(vfs_mount_table[i].path, path)) {
			return (int)i;
		}
	}
	return -1;
}

static int m9p_write_file(const char *path, const uint8_t *data,
			  uint16_t len)
{
	struct vfs_file file;
	int result;

	result = vfs_open(path, (uint16_t)(ZFS_O_WRITE | ZFS_O_CREATE |
					   ZFS_O_TRUNC), &file);
	if (result != 0) {
		return result;
	}
	result = vfs_write(&file, data, len);
	if (result != 0) {
		(void)vfs_close(&file);
		return result;
	}
	return vfs_close(&file);
}

/* Write a patterned file, read it back in one windowed pass and compare. */
static int m9p_round_trip(const char *path, uint16_t len)
{
	struct vfs_file file;
	uint16_t i;
	int result;

	m9p_pattern(m9p_ref, len);
	result = m9p_write_file(path, m9p_ref, len);
	if (result != 0) {
		return result;
	}
	if (vfs_stat(path, &m9p_stat) != 0 || m9p_stat.size != len ||
	    m9p_stat.type != ZFS_TYPE_FILE) {
		return -1;
	}
	result = vfs_open(path, ZFS_O_READ, &file);
	if (result != 0) {
		return result;
	}
	for (i = 0; i < len; ++i) {
		m9p_buf[i] = 0;
	}
	result = vfs_read(&file, m9p_buf, len);
	(void)vfs_close(&file);
	if (result != (int)len) {
		return -1;
	}
	for (i = 0; i < len; ++i) {
		if (m9p_buf[i] != m9p_ref[i]) {
			return -1;
		}
	}
	return 0;
}

static void m9p_test_registry(void)
{
	const struct zfs_provider *ramfs;
	const struct zfs_provider *probe;
	void *fs_state = (void *)0;

	if (vfs_phase() != VFS_PHASE_READY || module_count() == 0) {
		m9p_fail(ZOS_M9PST_FAIL_BOOT);
		return;
	}
	ramfs = vfs_find_provider("ramfs");
	probe = vfs_find_provider("probe");
	if (ramfs == (const struct zfs_provider *)0 ||
	    probe == (const struct zfs_provider *)0 ||
	    vfs_find_provider("failfs") != (const struct zfs_provider *)0 ||
	    module_failed_count() < 2) {
		m9p_fail(ZOS_M9PST_FAIL_REGISTRY);
		return;
	}
	/* The refuse-mount control: a present provider whose mount() fails
	 * is a mount failure, distinct from a missing provider. */
	if (ramfs->ops->mount(ramfs->provider_state, "refuse", &fs_state) !=
	    -ZOS_EIO) {
		m9p_fail(ZOS_M9PST_FAIL_MOUNT);
	}
}

static void m9p_test_mounts(void)
{
	int root = m9p_mount_index("/");
	int r2 = m9p_mount_index("/R2");

	if (root < 0 || r2 < 0) {
		m9p_fail(ZOS_M9PST_FAIL_MOUNT);
		return;
	}
	if (vfs_mount_state[root].provider == (const struct zfs_provider *)0 ||
	    vfs_mount_state[root].fs_state == (void *)0 ||
	    vfs_mount_state[r2].fs_state == (void *)0 ||
	    vfs_mount_state[root].fs_state == vfs_mount_state[r2].fs_state) {
		m9p_fail(ZOS_M9PST_FAIL_MOUNT);
	}
}

static void m9p_test_directories(void)
{
	if (vfs_mkdir("/DIR") != 0 || vfs_mkdir("/DIR/SUB") != 0) {
		m9p_fail(ZOS_M9PST_FAIL_MKDIR);
		return;
	}
	if (vfs_mkdir("/DIR") != -ZOS_EEXIST ||
	    vfs_rmdir("/DIR") != -ZOS_ENOTEMPTY) {
		m9p_fail(ZOS_M9PST_FAIL_MKDIR);
		return;
	}
	if (vfs_stat("/DIR", &m9p_stat) != 0 ||
	    m9p_stat.type != ZFS_TYPE_DIR) {
		m9p_fail(ZOS_M9PST_FAIL_MKDIR);
		return;
	}
	if (vfs_rmdir("/DIR/SUB") != 0 || vfs_rmdir("/DIR") != 0) {
		m9p_fail(ZOS_M9PST_FAIL_MKDIR);
		return;
	}
	if (vfs_stat("/DIR", &m9p_stat) != -ZOS_ENOENT) {
		m9p_fail(ZOS_M9PST_FAIL_MKDIR);
	}
}

static void m9p_test_open_semantics(void)
{
	struct vfs_file file;
	void *handle = (void *)0;
	uint32_t size = 0;
	int root = m9p_mount_index("/");

	if (root < 0) {
		m9p_fail(ZOS_M9PST_FAIL_MOUNT);
		return;
	}
	if (vfs_open("/", ZFS_O_READ, &file) != -ZOS_EISDIR) {
		m9p_fail(ZOS_M9PST_FAIL_OPEN);
		return;
	}
	/* Unknown flag bits, TRUNC without WRITE, and EXCL without CREATE
	 * are all EINVAL before the provider is consulted. */
	if (vfs_open("/X.TXT", (uint16_t)(ZFS_O_READ | 0x20u), &file) !=
		    -ZOS_EINVAL ||
	    vfs_open("/X.TXT", (uint16_t)(ZFS_O_READ | ZFS_O_TRUNC),
		     &file) != -ZOS_EINVAL ||
	    vfs_open("/X.TXT", (uint16_t)(ZFS_O_READ | ZFS_O_EXCL),
		     &file) != -ZOS_EINVAL) {
		m9p_fail(ZOS_M9PST_FAIL_OPEN);
		return;
	}
	/* Create through the provider directly so the root merge test has a
	 * real entry named like the mounted /R2.  The VFS would otherwise
	 * route `/R2` to the mountpoint. */
	{
		const struct zfs_ops *ops =
			vfs_mount_state[root].provider->ops;

		if (ops->open(vfs_mount_state[root].fs_state, "/R2",
			      (uint16_t)(ZFS_O_WRITE | ZFS_O_CREATE),
			      &handle, &size) != 0) {
			m9p_fail(ZOS_M9PST_FAIL_ROOTS);
			return;
		}
		(void)ops->write(vfs_mount_state[root].fs_state, handle, 0,
				 (const void *)m9p_ref, 0);
		(void)ops->close(vfs_mount_state[root].fs_state, handle);
	}
	/* A directory is not a file, and a file is not a directory. */
	{
		struct vfs_dir dir;

		if (vfs_mkdir("/D2") != 0 ||
		    vfs_open("/D2", ZFS_O_READ, &file) != -ZOS_EISDIR ||
		    m9p_write_file("/F.TXT", m9p_ref, 1) != 0 ||
		    vfs_opendir("/F.TXT", &dir) != -ZOS_ENOTDIR ||
		    vfs_opendir("/NOPE.TXT", &dir) != -ZOS_ENOENT) {
			m9p_fail(ZOS_M9PST_FAIL_OPEN);
			return;
		}
	}
	(void)vfs_rmdir("/D2");
	(void)vfs_unlink("/F.TXT");
}

static void m9p_test_rename(void)
{
	if (m9p_write_file("/A.TXT", m9p_ref, 3) != 0 ||
	    vfs_rename("/A.TXT", "/B.TXT") != 0) {
		m9p_fail(ZOS_M9PST_FAIL_RENAME);
		return;
	}
	if (vfs_stat("/A.TXT", &m9p_stat) != -ZOS_ENOENT ||
	    vfs_stat("/B.TXT", &m9p_stat) != 0) {
		m9p_fail(ZOS_M9PST_FAIL_RENAME);
		return;
	}
	/* Cross-mount rename is EXDEV; VFS never copies. */
	if (vfs_rename("/B.TXT", "/R2/B.TXT") != -ZOS_EXDEV) {
		m9p_fail(ZOS_M9PST_FAIL_EXDEV);
		return;
	}
	if (vfs_unlink("/B.TXT") != 0 || vfs_stat("/B.TXT", &m9p_stat) !=
	    -ZOS_ENOENT) {
		m9p_fail(ZOS_M9PST_FAIL_UNLINK);
	}
}

static void m9p_test_errors(void)
{
	if (vfs_stat("/", &m9p_stat) != 0 || m9p_stat.type != ZFS_TYPE_DIR) {
		m9p_fail(ZOS_M9PST_FAIL_ERRORS);
		return;
	}
	/* Read-only provider: every null mutation pointer is EROFS. */
	if (vfs_mkdir("/SYSTEM/x") != -ZOS_EROFS ||
	    vfs_unlink("/SYSTEM/monitor.zex") != -ZOS_EROFS) {
		m9p_fail(ZOS_M9PST_FAIL_ERRORS);
		return;
	}
	/* Mountpoints may not be removed or renamed, and mkdir of one is
	 * EEXIST (VFS answers, the provider is not consulted). */
	if (vfs_rmdir("/R2") != -ZOS_EBUSY ||
	    vfs_unlink("/R2") != -ZOS_EBUSY ||
	    vfs_rename("/R2", "/R3") != -ZOS_EBUSY ||
	    vfs_mkdir("/R2") != -ZOS_EEXIST) {
		m9p_fail(ZOS_M9PST_FAIL_ERRORS);
	}
}

static void m9p_test_root_merge(void)
{
	uint16_t r2_count = 0;
	uint8_t system_seen = 0;
	uint8_t hello_seen = 0;
	struct vfs_dir dir;

	if (m9p_write_file("/HELLO.TXT", m9p_ref, 2) != 0 ||
	    vfs_opendir("/", &dir) != 0) {
		m9p_fail(ZOS_M9PST_FAIL_ROOTS);
		return;
	}
	for (;;) {
		int result = vfs_readdir(&dir, &m9p_dirent);

		if (result == 0) {
			break;
		}
		if (result < 0) {
			(void)vfs_closedir(&dir);
			m9p_fail(ZOS_M9PST_FAIL_ROOTS);
			return;
		}
		if (vfs_name_equal(m9p_dirent.name, "R2")) {
			++r2_count;
			if (m9p_dirent.type != ZFS_TYPE_DIR) {
				(void)vfs_closedir(&dir);
				m9p_fail(ZOS_M9PST_FAIL_ROOTS);
				return;
			}
		} else if (vfs_name_equal(m9p_dirent.name, "SYSTEM")) {
			system_seen = 1;
		} else if (vfs_name_equal(m9p_dirent.name, "HELLO.TXT")) {
			hello_seen = 1;
		}
	}
	(void)vfs_closedir(&dir);
	/* The mountpoint shadows the backing entry: R2 appears exactly once
	 * and as the directory VFS owns. */
	if (r2_count != 1u || system_seen == 0 || hello_seen == 0) {
		m9p_fail(ZOS_M9PST_FAIL_ROOTS);
	}
}

static void m9p_test_sizes(void)
{
	static const struct {
		const char *name;
		uint16_t size;
	} cases[] = {
		{ "/SIZES/1", 1u },   { "/SIZES/127", 127u },
		{ "/SIZES/128", 128u }, { "/SIZES/129", 129u },
		{ "/SIZES/511", 511u }, { "/SIZES/512", 512u },
		{ "/SIZES/513", 513u }
	};
	uint8_t i;

	if (vfs_mkdir("/SIZES") != 0) {
		m9p_fail(ZOS_M9PST_FAIL_SIZES);
		return;
	}
	for (i = 0; i < (uint8_t)(sizeof(cases) / sizeof(cases[0])); ++i) {
		if (m9p_round_trip(cases[i].name, cases[i].size) != 0 ||
		    vfs_unlink(cases[i].name) != 0) {
			m9p_fail(ZOS_M9PST_FAIL_SIZES);
			return;
		}
	}
	if (vfs_rmdir("/SIZES") != 0) {
		m9p_fail(ZOS_M9PST_FAIL_SIZES);
	}
}

static void m9p_test_seek(void)
{
	struct vfs_file file;
	uint32_t pos = 0;

	if (m9p_write_file("/SEEK.TXT", m9p_ref, 5) != 0 ||
	    vfs_open("/SEEK.TXT", ZFS_O_READ, &file) != 0) {
		m9p_fail(ZOS_M9PST_FAIL_SEEK);
		return;
	}
	/* A 32-bit position past 64 KiB is representable; the read is a
	 * clean zero-byte EOF, not a wrap to the start. */
	if (vfs_seek(&file, 70000, ZFS_SEEK_SET, &pos) != 0 || pos != 70000u ||
	    vfs_read(&file, m9p_buf, 16) != 0) {
		(void)vfs_close(&file);
		m9p_fail(ZOS_M9PST_FAIL_SEEK);
		return;
	}
	if (vfs_seek(&file, -1, ZFS_SEEK_SET, (uint32_t *)0) != -ZOS_EINVAL) {
		(void)vfs_close(&file);
		m9p_fail(ZOS_M9PST_FAIL_SEEK);
		return;
	}
	if (vfs_seek(&file, 0, ZFS_SEEK_SET, &pos) != 0 || pos != 0u ||
	    vfs_read(&file, m9p_buf, 2) != 2) {
		(void)vfs_close(&file);
		m9p_fail(ZOS_M9PST_FAIL_SEEK);
		return;
	}
	if (vfs_seek(&file, -1, ZFS_SEEK_CUR, (uint32_t *)0) != 0 ||
	    vfs_seek(&file, -5, ZFS_SEEK_CUR, (uint32_t *)0) != -ZOS_EINVAL) {
		(void)vfs_close(&file);
		m9p_fail(ZOS_M9PST_FAIL_SEEK);
		return;
	}
	if (vfs_seek(&file, 0, ZFS_SEEK_END, &pos) != 0 || pos != 5u) {
		(void)vfs_close(&file);
		m9p_fail(ZOS_M9PST_FAIL_SEEK);
		return;
	}
	(void)vfs_close(&file);
	(void)vfs_unlink("/SEEK.TXT");
}

static void m9p_test_stale_and_failed_write(void)
{
	struct vfs_file file;
	uint32_t pos = 0;

	/* Stale handles: creating the /STALE control bumps the instance
	 * generation, so the already-open handle reports ESTALE. */
	if (m9p_write_file("/S.TXT", m9p_ref, 3) != 0 ||
	    vfs_open("/S.TXT", (uint16_t)(ZFS_O_WRITE | ZFS_O_TRUNC),
		     &file) != 0 ||
	    vfs_write(&file, m9p_ref, 3) != 0 ||
	    vfs_mkdir("/STALE") != 0) {
		m9p_fail(ZOS_M9PST_FAIL_STALE);
		return;
	}
	if (vfs_write(&file, m9p_ref, 1) != -ZOS_ESTALE ||
	    vfs_seek(&file, 0, ZFS_SEEK_CUR, &pos) != 0 || pos != 3u) {
		(void)vfs_close(&file);
		m9p_fail(ZOS_M9PST_FAIL_STALE);
		return;
	}
	(void)vfs_close(&file);
	if (vfs_rmdir("/STALE") != 0) {
		m9p_fail(ZOS_M9PST_FAIL_STALE);
		return;
	}

	/* A failed confirmed write: the /FAILWRITE control makes the next
	 * write fail with EIO, and neither the offset nor the size moves. */
	{
		struct vfs_file ctl;

		if (vfs_open("/W2.TXT",
			     (uint16_t)(ZFS_O_WRITE | ZFS_O_CREATE |
					ZFS_O_TRUNC), &file) != 0 ||
		    vfs_write(&file, m9p_ref, 4) != 0 ||
		    vfs_seek(&file, 2, ZFS_SEEK_SET, &pos) != 0 ||
		    vfs_open("/FAILWRITE",
			     (uint16_t)(ZFS_O_WRITE | ZFS_O_CREATE), &ctl) !=
			    0) {
			m9p_fail(ZOS_M9PST_FAIL_WRITEFAIL);
			return;
		}
		(void)vfs_close(&ctl);
		if (vfs_write(&file, m9p_ref, 2) != -ZOS_EIO ||
		    vfs_seek(&file, 0, ZFS_SEEK_CUR, &pos) != 0 || pos != 2u ||
		    vfs_stat("/W2.TXT", &m9p_stat) != 0 ||
		    m9p_stat.size != 4u) {
			(void)vfs_close(&file);
			m9p_fail(ZOS_M9PST_FAIL_WRITEFAIL);
			return;
		}
		(void)vfs_close(&file);
		(void)vfs_unlink("/FAILWRITE");
		(void)vfs_unlink("/W2.TXT");
	}
}

static void m9p_test_counters(void)
{
	if (vfs_open("/STATS", ZFS_O_READ, &m9p_file) != 0) {
		m9p_fail(ZOS_M9PST_FAIL_COUNTERS);
		return;
	}
	if (vfs_read(&m9p_file, &m9p_stats, (uint16_t)sizeof(m9p_stats)) !=
	    (int)sizeof(m9p_stats)) {
		(void)vfs_close(&m9p_file);
		m9p_fail(ZOS_M9PST_FAIL_COUNTERS);
		return;
	}
	(void)vfs_close(&m9p_file);
	if (m9p_stats.open_handles < 1u ||
	    m9p_stats.calls[1] < 1u ||  /* open  */
	    m9p_stats.calls[4] < 1u ||  /* write */
	    m9p_stats.calls[10] < 1u) { /* mkdir */
		m9p_fail(ZOS_M9PST_FAIL_COUNTERS);
	}
}

void zos_run_m9_provider_selftest(void)
{
	m9_provider_selftest_result = ZOS_M9PST_FAIL_UNKNOWN;

	m9p_test_registry();
	if (m9_provider_selftest_result != ZOS_M9PST_FAIL_UNKNOWN) {
		return;
	}
	m9p_test_mounts();
	if (m9_provider_selftest_result != ZOS_M9PST_FAIL_UNKNOWN) {
		return;
	}
	m9p_test_directories();
	if (m9_provider_selftest_result != ZOS_M9PST_FAIL_UNKNOWN) {
		return;
	}
	m9p_test_open_semantics();
	if (m9_provider_selftest_result != ZOS_M9PST_FAIL_UNKNOWN) {
		return;
	}
	m9p_test_rename();
	if (m9_provider_selftest_result != ZOS_M9PST_FAIL_UNKNOWN) {
		return;
	}
	m9p_test_errors();
	if (m9_provider_selftest_result != ZOS_M9PST_FAIL_UNKNOWN) {
		return;
	}
	m9p_test_root_merge();
	if (m9_provider_selftest_result != ZOS_M9PST_FAIL_UNKNOWN) {
		return;
	}
	m9p_test_sizes();
	if (m9_provider_selftest_result != ZOS_M9PST_FAIL_UNKNOWN) {
		return;
	}
	m9p_test_seek();
	if (m9_provider_selftest_result != ZOS_M9PST_FAIL_UNKNOWN) {
		return;
	}
	m9p_test_stale_and_failed_write();
	if (m9_provider_selftest_result != ZOS_M9PST_FAIL_UNKNOWN) {
		return;
	}
	m9p_test_counters();
	if (m9_provider_selftest_result != ZOS_M9PST_FAIL_UNKNOWN) {
		return;
	}

	m9_provider_selftest_result = ZOS_M9PST_OK;
	early_console_puts("m9 provider selftest: OK\r\n");
}

#endif /* ZOS_M9_PROVIDER_SELFTEST */
