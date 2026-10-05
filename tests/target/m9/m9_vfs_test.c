/* ZephyrOS M9 VFS-bootstrap acceptance selftest (test artifact).
 *
 * Runs from the late acceptance hook after the production-style VFS boot:
 * vfs_init() mounted ROMFS at /SYSTEM, the resident modules and the monitor
 * were loaded through VFS, and vfs_mount_all() consumed a test manifest
 * whose optional mount names an unregistered provider.
 *
 * Covers (work package 25, correction-02 -- path-based providers):
 *   - the optional-mount failure state and its ZOS_ENODEV code;
 *   - flat mount selection: `/SYSTEMX` must not match `/SYSTEM`;
 *   - stat("/") and stat("/SYSTEM") answered by VFS as directories;
 *   - lookup, stat and read of every validated ROMFS entry through VFS,
 *     byte-compared against the reader the provider sits on;
 *   - case-insensitive resolution and '.'/'..'/'//' normalization;
 *   - ENOENT, ENOTDIR (a deeper path under a file) and the deeper-path
 *     ENOENT case (a deeper path under an unknown first component);
 *   - EROFS from the read-only provider: write flags at open, the null
 *     mutation operations through the VFS entry points, and the provider's
 *     null pointers directly;
 *   - root enumeration with `/` unmounted (only SYSTEM);
 *   - the VFS executable gate used by ZEX loading (monitor.zex accepted, a
 *     DATA resource refused).
 *
 * Prints "m9 vfs selftest: OK" through the console service on success; a
 * precise FAIL marker with the result code otherwise.
 */

#include "zos/fs.h"
#include "zephyros/vfs.h"
#include "zephyros/romfs.h"
#include "zephyros/module.h"
#include "zephyros/console.h"
#include "zephyros/kernel.h"
#include "kernel/vfs/vfs.h"

#include "m9_selftest.h"

#if ZOS_M9_VFS_SELFTEST

uint8_t m9_vfs_selftest_result;

static uint8_t m9_buf[64];
static uint8_t m9_ref[64];
static char m9_path[ZOS_PATH_MAX];
static char m9_path2[ZOS_PATH_MAX];
static struct zfs_dirent m9_dirent;
static struct vfs_file m9_file;

/* Diagnostics go over the direct early SIO so the transcript is
 * independent of whichever console the boot bound. */
static void m9_put_hex8(uint8_t value)
{
	static const char digits[] = "0123456789ABCDEF";

	early_console_putc(digits[(value >> 4) & 0x0Fu]);
	early_console_putc(digits[value & 0x0Fu]);
}

static void m9_fail(uint8_t code)
{
	m9_vfs_selftest_result = code;
	early_console_puts("m9 vfs selftest: FAIL 0x");
	m9_put_hex8(code);
	early_console_puts("\r\n");
}

static char *m9_append(char *out, const char *text)
{
	while (*text != '\0') {
		*out++ = *text++;
	}
	*out = '\0';
	return out;
}

static uint16_t m9_strlen(const char *text)
{
	uint16_t n = 0;

	while (text[n] != '\0') {
		++n;
	}
	return n;
}

/* "/SYSTEM/<name>" or "<prefix><name>" into `dst`. */
static const char *m9_make_path(char *dst, const char *prefix, const char *name)
{
	char *out = m9_append(dst, prefix);

	(void)m9_append(out, name);
	return dst;
}

/* Copy a literal path into `dst`. */
static const char *m9_set_path(char *dst, const char *text)
{
	(void)m9_append(dst, text);
	return dst;
}

static uint8_t m9_check_entry(const char *name, const romfs_file_t *ref)
{
	struct zfs_stat st;
	struct vfs_file file;
	uint32_t off = 0;
	int result;

	m9_make_path(m9_path, "/SYSTEM/", name);
	result = vfs_stat(m9_path, &st);
	if (result != 0 || st.type != ZFS_TYPE_FILE || st.size != ref->size ||
	    (st.attributes & ZFS_ATTR_READONLY) == 0) {
		return ZOS_M9ST_FAIL_ENTRY;
	}
	result = vfs_open(m9_path, ZFS_O_READ, &file);
	if (result != 0 || file.size != ref->size) {
		return ZOS_M9ST_FAIL_ENTRY;
	}
	while (off < ref->size) {
		uint16_t chunk = 64;
		uint16_t i;

		if ((uint32_t)chunk > (ref->size - off)) {
			chunk = (uint16_t)(ref->size - off);
		}
		if (vfs_seek(&file, (int32_t)off, ZFS_SEEK_SET,
			     (uint32_t *)0) != 0 ||
		    vfs_read(&file, m9_buf, chunk) != (int)chunk ||
		    romfs_read(ref, off, m9_ref, chunk) != (int)chunk) {
			(void)vfs_close(&file);
			return ZOS_M9ST_FAIL_ENTRY;
		}
		for (i = 0; i < chunk; ++i) {
			if (m9_buf[i] != m9_ref[i]) {
				(void)vfs_close(&file);
				return ZOS_M9ST_FAIL_ENTRY;
			}
		}
		off += chunk;
	}
	(void)vfs_close(&file);
	return ZOS_M9ST_OK;
}

/* Enumerate the validated directory through the reader (the provider's
 * own `readdir` path) and report the first entry with `flags`. */
static uint8_t m9_find_entry(uint16_t flags, char *name, uint16_t name_size,
			     romfs_file_t *out)
{
	uint16_t i = 0;
	romfs_file_t file;

	while (romfs_entry_at(i, &file, name, name_size) == 0) {
		if (file.flags == flags) {
			*out = file;
			return 1;
		}
		++i;
	}
	return 0;
}

void zos_run_m9_vfs_selftest(void)
{
	struct zfs_stat st;
	struct vfs_dir dir;
	romfs_file_t ref;
	char first_name[ROMFS_NAME_SIZE];
	uint8_t have_data = 0;
	uint8_t have_zex = 0;
	uint8_t code;
	uint16_t i = 0;

	m9_vfs_selftest_result = ZOS_M9ST_FAIL_BOOT;

	/* Boot baseline: VFS reached READY, a module is resident, the console
	 * is bound and the recovery store published at least one entry. */
	if (vfs_phase() != VFS_PHASE_READY || module_count() == 0 ||
	    console_output_handle() == 0 ||
	    romfs_entry_at(0, &ref, first_name, ROMFS_NAME_SIZE) != 0) {
		m9_fail(ZOS_M9ST_FAIL_BOOT);
		return;
	}

	/* Optional-mount failure: the test manifest's /DATA names an
	 * unregistered provider, so its runtime entry is FAILED/ENODEV and
	 * contributes nothing to the namespace. */
	{
		uint8_t found = 0;
		uint16_t mi = 0;

		while (mi < vfs_mount_count) {
			if ((vfs_mount_table[mi].flags & VFS_MOUNT_SYSTEM) == 0) {
				if (vfs_mount_state[mi].state != VFS_MOUNT_FAILED ||
				    vfs_mount_state[mi].last_error != -ZOS_ENODEV) {
					m9_fail(ZOS_M9ST_FAIL_MOUNT);
					return;
				}
				found = 1;
			}
			++mi;
		}
		if (!found) {
			m9_fail(ZOS_M9ST_FAIL_MOUNT);
			return;
		}
	}
	if (vfs_stat("/DATA", &st) != -ZOS_ENODEV) {
		m9_fail(ZOS_M9ST_FAIL_MOUNT);
		return;
	}

	/* stat("/") is the VFS-owned root directory. */
	if (vfs_stat("/", &st) != 0 || st.type != ZFS_TYPE_DIR) {
		m9_fail(ZOS_M9ST_FAIL_ROOT);
		return;
	}
	/* stat("/SYSTEM") is a mounted mountpoint, answered by VFS as a
	 * directory. */
	if (vfs_stat("/SYSTEM", &st) != 0 || st.type != ZFS_TYPE_DIR) {
		m9_fail(ZOS_M9ST_FAIL_SYSTEM);
		return;
	}

	/* Flat mount selection: `/SYSTEMX` is a different first component, so
	 * it must not select `/SYSTEM`.  With no filesystem at `/` and no
	 * such entry in the recovery store the path is unowned (ENODEV) --
	 * either way it must not resolve. */
	if (vfs_stat("/SYSTEMX", &st) == 0) {
		m9_fail(ZOS_M9ST_FAIL_FLAT);
		return;
	}
	if (vfs_stat("/SYSTEM/monitor.zex", &st) == 0 &&
	    st.type != ZFS_TYPE_FILE) {
		m9_fail(ZOS_M9ST_FAIL_FLAT);
		return;
	}

	/* Lookup, stat and read every validated ROMFS entry; entry 0's name
	 * is kept for the case, normalization and deeper-path cases. */
	first_name[0] = '\0';
	{
		char name[ROMFS_NAME_SIZE];

		while (romfs_entry_at(i, &ref, name, ROMFS_NAME_SIZE) == 0) {
			if (i == 0) {
				(void)m9_append(first_name, name);
			}
			code = m9_check_entry(name, &ref);
			if (code != ZOS_M9ST_OK) {
				m9_fail(code);
				return;
			}
			if (ref.flags == ROMFS_FLAG_DATA) {
				have_data = 1;
			} else if (ref.flags == ROMFS_FLAG_ZEX) {
				have_zex = 1;
			}
			++i;
		}
	}
	if (i == 0) {
		m9_fail(ZOS_M9ST_FAIL_ENTRY);
		return;
	}

	/* Case-insensitive resolution of the mountpoint and the component. */
	m9_make_path(m9_path, "/system/", first_name);
	if (vfs_stat(m9_path, &st) != 0) {
		m9_fail(ZOS_M9ST_FAIL_CASE);
		return;
	}
	{
		char *p = m9_path;

		while (*p != '\0') {
			if (*p >= 'a' && *p <= 'z') {
				*p = (char)(*p - 32);
			}
			++p;
		}
	}
	if (vfs_stat(m9_path, &st) != 0) {
		m9_fail(ZOS_M9ST_FAIL_CASE);
		return;
	}

	/* Normalization: repeated '/', '.', '..' and a relative path. */
	m9_make_path(m9_path, "/SYSTEM//./", first_name);
	if (vfs_stat(m9_path, &st) != 0) {
		m9_fail(ZOS_M9ST_FAIL_NORMALIZE);
		return;
	}
	m9_make_path(m9_path, "/SYSTEM/../SYSTEM/", first_name);
	if (vfs_stat(m9_path, &st) != 0) {
		m9_fail(ZOS_M9ST_FAIL_NORMALIZE);
		return;
	}
	m9_make_path(m9_path, "SYSTEM/", first_name);
	if (vfs_stat(m9_path, &st) != 0) {
		m9_fail(ZOS_M9ST_FAIL_NORMALIZE);
		return;
	}

	/* ENOENT for a missing name. */
	m9_make_path(m9_path, "/SYSTEM/", "absent-xyz");
	if (vfs_stat(m9_path, &st) != -ZOS_ENOENT) {
		m9_fail(ZOS_M9ST_FAIL_ENOENT);
		return;
	}

	/* Deeper paths: ENOTDIR when the first component names a file, and
	 * ENOENT when it names nothing. */
	m9_make_path(m9_path, "/SYSTEM/", first_name);
	(void)m9_append(m9_path + m9_strlen(m9_path), "/child");
	if (vfs_stat(m9_path, &st) != -ZOS_ENOTDIR) {
		m9_fail(ZOS_M9ST_FAIL_ENOTDIR);
		return;
	}
	if (vfs_stat(m9_set_path(m9_path, "/SYSTEM/absent-xyz/child"), &st) !=
	    -ZOS_ENOENT) {
		m9_fail(ZOS_M9ST_FAIL_DEEPER);
		return;
	}
	/* The same rule through open. */
	if (vfs_open(m9_set_path(m9_path, "/SYSTEM/absent-xyz/child"),
		     ZFS_O_READ, &m9_file) != -ZOS_ENOENT) {
		m9_fail(ZOS_M9ST_FAIL_DEEPER);
		return;
	}
	m9_make_path(m9_path, "/SYSTEM/", first_name);
	(void)m9_append(m9_path + m9_strlen(m9_path), "/child");
	if (vfs_opendir(m9_path, &dir) != -ZOS_ENOTDIR) {
		m9_fail(ZOS_M9ST_FAIL_DEEPER);
		return;
	}
	/* `/SYSTEM/<file>` is not a directory either. */
	m9_make_path(m9_path, "/SYSTEM/", first_name);
	if (vfs_opendir(m9_path, &dir) != -ZOS_ENOTDIR) {
		m9_fail(ZOS_M9ST_FAIL_DEEPER);
		return;
	}
	/* A mountpoint is a directory; opening it as a file is EISDIR. */
	if (vfs_open("/SYSTEM", ZFS_O_READ, &m9_file) != -ZOS_EISDIR) {
		m9_fail(ZOS_M9ST_FAIL_DEEPER);
		return;
	}

	/* EROFS: the provider is read-only, so write intent is refused at
	 * open and every mutation entry point answers EROFS (the mutation
	 * pointers are null; VFS applies the rule in one place). */
	m9_make_path(m9_path, "/SYSTEM/", first_name);
	if (vfs_open(m9_path, (uint16_t)(ZFS_O_READ | ZFS_O_WRITE),
		     &m9_file) != -ZOS_EROFS) {
		m9_fail(ZOS_M9ST_FAIL_EROFS);
		return;
	}
	/* TRUNC without WRITE is a flag error, rejected by VFS before the
	 * provider is consulted (architecture section 32.28). */
	if (vfs_open(m9_path, (uint16_t)(ZFS_O_READ | ZFS_O_TRUNC),
		     &m9_file) != -ZOS_EINVAL) {
		m9_fail(ZOS_M9ST_FAIL_FLAGS);
		return;
	}
	/* Unknown flag bits are rejected too. */
	if (vfs_open(m9_path, (uint16_t)(ZFS_O_READ | 0x20u),
		     &m9_file) != -ZOS_EINVAL) {
		m9_fail(ZOS_M9ST_FAIL_FLAGS);
		return;
	}
	if (vfs_mkdir(m9_path) != -ZOS_EROFS ||
	    vfs_rmdir(m9_path) != -ZOS_EROFS ||
	    vfs_unlink(m9_path) != -ZOS_EROFS) {
		m9_fail(ZOS_M9ST_FAIL_EROFS);
		return;
	}
	m9_make_path(m9_path2, "/SYSTEM/", "newname");
	if (vfs_rename(m9_path, m9_path2) != -ZOS_EROFS) {
		m9_fail(ZOS_M9ST_FAIL_EROFS);
		return;
	}
	/* ... and the null pointers are what carries that rule. */
	{
		const struct zfs_provider *provider = vfs_find_provider("romfs");

		if (provider == (const struct zfs_provider *)0 ||
		    provider->ops->write != (void *)0 ||
		    provider->ops->sync != (void *)0 ||
		    provider->ops->mkdir != (void *)0 ||
		    provider->ops->rmdir != (void *)0 ||
		    provider->ops->unlink != (void *)0 ||
		    provider->ops->rename != (void *)0) {
			m9_fail(ZOS_M9ST_FAIL_MUTATE);
			return;
		}
	}

	/* Root enumeration with `/` unmounted: exactly the mounted
	 * root-level mountpoints, and here only SYSTEM. */
	if (vfs_opendir("/", &dir) != 0) {
		m9_fail(ZOS_M9ST_FAIL_READDIR);
		return;
	}
	{
		int result = vfs_readdir(&dir, &m9_dirent);
		uint8_t n = 0;

		while (n < 8 && m9_dirent.name[n] != '\0') {
			++n;
		}
		if (result != 1 || m9_dirent.type != ZFS_TYPE_DIR ||
		    !vfs_name_equal(m9_dirent.name, "SYSTEM")) {
			(void)vfs_closedir(&dir);
			m9_fail(ZOS_M9ST_FAIL_READDIR);
			return;
		}
		if (vfs_readdir(&dir, &m9_dirent) != 0) {
			(void)vfs_closedir(&dir);
			m9_fail(ZOS_M9ST_FAIL_READDIR);
			return;
		}
	}
	(void)vfs_closedir(&dir);

	/* The directory enumerates exactly the ROMFS entries, in image
	 * order, through the provider path. */
	if (vfs_opendir("/SYSTEM", &dir) != 0) {
		m9_fail(ZOS_M9ST_FAIL_READDIR);
		return;
	}
	i = 0;
	{
		romfs_file_t entry;
		char name[ROMFS_NAME_SIZE];
		int result;

		while ((result = vfs_readdir(&dir, &m9_dirent)) == 1) {
			if (romfs_entry_at(i, &entry, name, ROMFS_NAME_SIZE) != 0 ||
			    m9_dirent.type != ZFS_TYPE_FILE ||
			    m9_dirent.size != entry.size ||
			    !vfs_name_equal(m9_dirent.name, name)) {
				(void)vfs_closedir(&dir);
				m9_fail(ZOS_M9ST_FAIL_READDIR);
				return;
			}
			++i;
		}
		if (result != 0 ||
		    romfs_entry_at(i, &entry, name, ROMFS_NAME_SIZE) == 0) {
			(void)vfs_closedir(&dir);
			m9_fail(ZOS_M9ST_FAIL_READDIR);
			return;
		}
	}
	(void)vfs_closedir(&dir);

	/* Executable gate: a ZEX opens with VFS_O_EXEC; a DATA resource is
	 * refused (preserving the M6 ZEX_EXEC_ERR_TYPE rejection). */
	if (!have_zex ||
	    !m9_find_entry(ROMFS_FLAG_ZEX, m9_path2, ZFS_NAME_MAX, &ref)) {
		m9_fail(ZOS_M9ST_FAIL_EXEC);
		return;
	}
	m9_make_path(m9_path, "/SYSTEM/", m9_path2);
	if (vfs_open(m9_path, (uint16_t)(ZFS_O_READ | VFS_O_EXEC),
		     &m9_file) != 0 || m9_file.size != ref.size) {
		m9_fail(ZOS_M9ST_FAIL_EXEC);
		return;
	}
	(void)vfs_close(&m9_file);

	if (!have_data ||
	    !m9_find_entry(ROMFS_FLAG_DATA, m9_path2, ZFS_NAME_MAX, &ref)) {
		m9_fail(ZOS_M9ST_FAIL_EXEC);
		return;
	}
	m9_make_path(m9_path, "/SYSTEM/", m9_path2);
	if (vfs_open(m9_path, (uint16_t)(ZFS_O_READ | VFS_O_EXEC),
		     &m9_file) != -ZOS_EINVAL) {
		m9_fail(ZOS_M9ST_FAIL_EXEC);
		return;
	}

	m9_vfs_selftest_result = ZOS_M9ST_OK;
	early_console_puts("m9 vfs selftest: OK\r\n");
}

#endif /* ZOS_M9_VFS_SELFTEST */
