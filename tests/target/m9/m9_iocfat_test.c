/* ZephyrOS M9 IOC-FAT provider acceptance (target, test only).
 *
 * Runs in the m9-iocfat-selftest image after boot, mount and console
 * binding.  The production `config/mounts.json` attaches `/` to the resident
 * `iocfat` provider (optional); this test drives it through the ordinary
 * kernel VFS so the same binary runs against a real SD card.
 *
 * Coverage: `/` served by iocfat, the open-mode set, read/write, truncate,
 * stat, single- and cross-block reads, same-directory rename and
 * cross-directory rename -> EXDEV, mkdir/rmdir (including ENOTEMPTY),
 * unlink, readdir, directory-open EISDIR, open-handle exhaustion -> EMFILE,
 * and the staleness probe.  A card whose CAPS report READ_ONLY takes a
 * separate branch that proves every mutation returns EROFS.  A writable card
 * then runs the native `fstest.zex`, whose IOC-FAT section covers the 8.3
 * rejection table, the cache rules and the resolver sequence through the
 * ordinary syscall path.
 *
 * Everything is written under a `/ZOSTEST` directory the test creates and
 * removes, so a hardware run never touches the user's data.
 *
 * Diagnostics use the direct early console so the transcript does not depend
 * on which console the boot bound.
 */

#include "zos/fs.h"
#include "zephyros/kernel.h"
#include "zephyros/process.h"

#include "kernel/vfs/vfs.h"
#include "kernel/lib/name.h"

#include "m9_iocfat_selftest.h"

#if ZOS_M9_IOCFAT_SELFTEST

uint8_t m9_iocfat_selftest_result;

#define M9I_ROOT  "/ZOSTEST"
#define M9I_A     "/ZOSTEST/A.TXT"
#define M9I_B     "/ZOSTEST/B.TXT"
#define M9I_ST    "/ZOSTEST/ST.TXT"
#define M9I_D1    "/ZOSTEST/D1"
#define M9I_D2    "/ZOSTEST/D2"
#define M9I_C1    "/ZOSTEST/D1/C.TXT"
#define M9I_C2    "/ZOSTEST/D2/C.TXT"
#define M9I_RO    "/RO.TXT"

static uint8_t m9i_buf[520];
/* One 512-byte period of the data pattern: the files the cases write repeat
 * it, so a read slice anywhere is checked against `offset & 511`. */
static uint8_t m9i_ref[520];
static struct zfs_stat m9i_stat;
static struct zfs_dirent m9i_dirent;
static struct vfs_file m9i_file;
static struct vfs_dir m9i_dir;

static void m9i_puts(const char *s)
{
	early_console_puts(s);
}

static void m9i_hex8(uint8_t value)
{
	static const char hex[] = "0123456789ABCDEF";

	early_console_putc(hex[(value >> 4) & 0x0Fu]);
	early_console_putc(hex[value & 0x0Fu]);
}

static void m9i_fail(uint8_t code)
{
	m9_iocfat_selftest_result = code;
	m9i_puts("m9 iocfat selftest: FAIL 0x");
	m9i_hex8(code);
	m9i_puts("\r\n");
}

static void m9i_note(const char *s)
{
	m9i_puts("m9 iocfat: ");
	m9i_puts(s);
	m9i_puts("\r\n");
}

static void m9i_pattern(uint8_t *buf, uint16_t len)
{
	uint16_t i;

	for (i = 0; i < len; ++i) {
		buf[i] = (uint8_t)((uint16_t)i * 29u + 11u);
	}
}

/* Write a 512-periodic file through create+truncate.  Returns 0 or a
 * negative error. */
static int m9i_write_file(const char *path, uint16_t len)
{
	uint16_t done = 0u;
	int result;

	m9i_pattern(m9i_ref, 512u);
	result = vfs_open(path, (uint16_t)(ZFS_O_WRITE | ZFS_O_CREATE |
					   ZFS_O_TRUNC), &m9i_file);
	if (result != 0) {
		return result;
	}
	while (done < len) {
		uint16_t chunk = (uint16_t)(len - done);

		if (chunk > 512u) {
			chunk = 512u;
		}
		if (vfs_write(&m9i_file, m9i_ref, chunk) != 0) {
			(void)vfs_close(&m9i_file);
			return -ZOS_EIO;
		}
		done = (uint16_t)(done + chunk);
	}
	return vfs_close(&m9i_file);
}

/* Compare a read slice against the periodic reference, so a file longer
 * than the buffer needs no longer reference. */
static uint8_t m9i_slice_diff(const uint8_t *buf, uint16_t offset,
			      uint16_t len)
{
	uint16_t i;

	for (i = 0; i < len; ++i) {
		if (buf[i] != m9i_ref[(uint16_t)(offset + i) & 511u]) {
			return 1u;
		}
	}
	return 0u;
}

/* Read `len` bytes at `offset` into m9i_buf.  Returns the byte count or a
 * negative error. */
static int m9i_read_at(const char *path, uint32_t offset, uint16_t len)
{
	int result;

	result = vfs_open(path, ZFS_O_READ, &m9i_file);
	if (result != 0) {
		return result;
	}
	result = vfs_seek(&m9i_file, (int32_t)offset, ZFS_SEEK_SET,
			  (uint32_t *)0);
	if (result != 0) {
		(void)vfs_close(&m9i_file);
		return result;
	}
	result = vfs_read(&m9i_file, m9i_buf, len);
	(void)vfs_close(&m9i_file);
	return result;
}

/* Static equality check without pulling in a library compare. */
static uint8_t m9i_diff(const uint8_t *a, const uint8_t *b, uint16_t len)
{
	uint16_t i;

	for (i = 0; i < len; ++i) {
		if (a[i] != b[i]) {
			return 1u;
		}
	}
	return 0u;
}

/* 0 when `/` is served by iocfat, 1 when the card is absent (an optional
 * mount must leave `/SYSTEM` and the console usable), -1 on a failure that
 * is not the documented media error. */
static int m9i_case_mount(void)
{
	uint16_t i;
	int index = -1;

	for (i = 0; i < vfs_mount_count; ++i) {
		if (zos_ascii_equal(vfs_mount_table[i].path, "/", 0u, 1u)) {
			index = (int)i;
		}
	}
	if (index < 0) {
		m9i_fail(ZOS_M9IOFAT_FAIL_MOUNT);
		return -1;
	}
	if (vfs_mount_state[index].state != VFS_MOUNT_MOUNTED) {
		if (vfs_mount_state[index].state == VFS_MOUNT_FAILED &&
		    vfs_mount_state[index].last_error == -ZOS_ENODEV) {
			m9i_note("card absent: / not mounted");
			return 1;
		}
		m9i_fail(ZOS_M9IOFAT_FAIL_MOUNT);
		return -1;
	}
	if (vfs_mount_state[index].provider == (const struct zfs_provider *)0 ||
	    vfs_mount_state[index].fs_state == (void *)0 ||
	    !zos_ascii_equal(vfs_mount_state[index].provider->name, "iocfat",
			     0u, 1u)) {
		m9i_fail(ZOS_M9IOFAT_FAIL_MOUNT);
		return -1;
	}
	return 0;
}

/* The provider's 8.3 rule.  Every name here must fail with EINVAL, and the
 * simulator asserts that no FS2 command at all was sent in the window. */
static int m9i_case_names(void)
{
	static const char *const bad[] = {
		"/ZOSTEST/ABCDEFGHI.TXT",	/* base over 8 bytes      */
		"/ZOSTEST/ABCDEFGH.TXTT",	/* extension over 3 bytes */
		"/ZOSTEST/.TXT",		/* empty base             */
		"/ZOSTEST/A.TXT.B",		/* second dot             */
		"/ZOSTEST/A+B.TXT",		/* character FatFs rejects */
		"/ZOSTEST/A B.TXT",		/* space (below 0x21)     */
		"/ZOSTEST/A~B.TXT"		/* '~' is in the reject set */
	};
	static const char *const deep16 =
		"/A/B/C/D/E/F/G/H/I/J/K/L/M/N/O/P";
	static const char *const deep18 =
		"/A/B/C/D/E/F/G/H/I/J/K/L/M/N/O/P/Q/R";
	uint16_t i;

	m9i_note("names:begin");
	for (i = 0; i < (uint16_t)(sizeof(bad) / sizeof(bad[0])); ++i) {
		if (vfs_stat(bad[i], &m9i_stat) != -ZOS_EINVAL ||
		    vfs_open(bad[i], ZFS_O_READ, &m9i_file) != -ZOS_EINVAL) {
			m9i_fail(ZOS_M9IOFAT_FAIL_NAME);
			return -1;
		}
	}
	m9i_note("names:end");

	/* The boundaries that must pack: an 8-byte base with a 3-byte
	 * extension, the folded lowercase spelling, and the padded readdir
	 * name coming back at its source spelling. */
	if (m9i_write_file("/ZOSTEST/ABCDEFGH.TXT", 8u) != 0 ||
	    vfs_open("/zostest/abcdefgh.txt", ZFS_O_READ, &m9i_file) != 0 ||
	    vfs_read(&m9i_file, m9i_buf, 8u) != 8 ||
	    m9i_diff(m9i_buf, m9i_ref, 8u) != 0u) {
		m9i_fail(ZOS_M9IOFAT_FAIL_NAME);
		return -1;
	}
	(void)vfs_close(&m9i_file);
	{
		uint8_t found = 0u;

		if (vfs_opendir(M9I_ROOT, &m9i_dir) != 0) {
			m9i_fail(ZOS_M9IOFAT_FAIL_NAME);
			return -1;
		}
		for (i = 0; i < 16u; ++i) {
			int r = vfs_readdir(&m9i_dir, &m9i_dirent);

			if (r <= 0) {
				break;
			}
			if (m9i_dirent.name_length == 13u &&
			    m9i_dirent.size == 8u &&
			    m9i_dirent.name[0] == 'A' &&
			    zos_ascii_equal(m9i_dirent.name,
					   "ABCDEFGH.TXT", 0u, 1u)) {
				found = 1u;
			}
		}
		(void)vfs_closedir(&m9i_dir);
		if (found == 0u) {
			m9i_fail(ZOS_M9IOFAT_FAIL_NAME);
			return -1;
		}
	}
	if (vfs_unlink("/ZOSTEST/ABCDEFGH.TXT") != 0) {
		m9i_fail(ZOS_M9IOFAT_FAIL_NAME);
		return -1;
	}

	/* Resolver sequencing: 16 components are walked, 18 are refused by
	 * the codec before the first command. */
	if (vfs_stat(deep16, &m9i_stat) != -ZOS_ENOENT ||
	    vfs_stat(deep18, &m9i_stat) != -ZOS_EINVAL) {
		m9i_fail(ZOS_M9IOFAT_FAIL_NAME);
		return -1;
	}
	return 0;
}

/* Every cache rule, fenced by notes the simulator counts FS2 commands
 * between.  Each window has exactly one expected FS2_READ count.  Every read
 * re-seeks first: the descriptor offset is the caller's, and a cache rule
 * must not depend on it. */
static int m9i_case_cache(void)
{
	struct vfs_file a;
	struct vfs_file b;
	int result;

	/* 1040 bytes: two full blocks and a partial third. */
	if (m9i_write_file(M9I_A, 1040u) != 0) {
		m9i_fail(ZOS_M9IOFAT_FAIL_WRITE);
		return -1;
	}
	if (vfs_open(M9I_A, ZFS_O_READ, &a) != 0) {
		m9i_fail(ZOS_M9IOFAT_FAIL_OPEN);
		return -1;
	}
	m9i_note("cache:0");
	if (vfs_read(&a, m9i_buf, 16u) != 16 ||
	    m9i_diff(m9i_buf, m9i_ref, 16u) != 0u) {
		m9i_fail(ZOS_M9IOFAT_FAIL_COMPARE);
		return -1;
	}
	m9i_note("cache:1");		/* +1 FS2_READ: block 0 */
	(void)vfs_seek(&a, 0, ZFS_SEEK_SET, (uint32_t *)0);
	if (vfs_read(&a, m9i_buf, 16u) != 16 ||
	    m9i_diff(m9i_buf, m9i_ref, 16u) != 0u) {
		m9i_fail(ZOS_M9IOFAT_FAIL_COMPARE);
		return -1;
	}
	m9i_note("cache:2");		/* +0: the slot is reused */
	(void)vfs_seek(&a, 500, ZFS_SEEK_SET, (uint32_t *)0);
	if (vfs_read(&a, m9i_buf, 64u) != 64 ||
	    m9i_slice_diff(m9i_buf, 500u, 64u) != 0u) {
		m9i_fail(ZOS_M9IOFAT_FAIL_COMPARE);
		return -1;
	}
	m9i_note("cache:3");		/* +1: block 1 joins block 0 */
	(void)vfs_seek(&a, 1024, ZFS_SEEK_SET, (uint32_t *)0);
	if (vfs_read(&a, m9i_buf, 16u) != 16 ||
	    m9i_slice_diff(m9i_buf, 1024u, 16u) != 0u) {
		m9i_fail(ZOS_M9IOFAT_FAIL_COMPARE);
		return -1;
	}
	m9i_note("cache:4");		/* +1: block 2 */
	(void)vfs_seek(&a, 0, ZFS_SEEK_SET, (uint32_t *)0);
	if (vfs_read(&a, m9i_buf, 16u) != 16 ||
	    m9i_diff(m9i_buf, m9i_ref, 16u) != 0u) {
		m9i_fail(ZOS_M9IOFAT_FAIL_COMPARE);
		return -1;
	}
	m9i_note("cache:5");		/* +1: block 0 again */
	(void)vfs_close(&a);

	/* A second writable handle owns the slot; its own confirmed write
	 * drops it. */
	if (vfs_open(M9I_A, ZFS_O_WRITE, &b) != 0) {
		m9i_fail(ZOS_M9IOFAT_FAIL_OPEN);
		return -1;
	}
	if (vfs_read(&b, m9i_buf, 16u) != 16 ||
	    m9i_diff(m9i_buf, m9i_ref, 16u) != 0u) {
		m9i_fail(ZOS_M9IOFAT_FAIL_COMPARE);
		return -1;
	}
	m9i_note("cache:6");		/* +1: the slot changed owner */
	(void)vfs_seek(&b, 0, ZFS_SEEK_SET, (uint32_t *)0);
	if (vfs_read(&b, m9i_buf, 16u) != 16 ||
	    m9i_diff(m9i_buf, m9i_ref, 16u) != 0u) {
		m9i_fail(ZOS_M9IOFAT_FAIL_COMPARE);
		return -1;
	}
	m9i_note("cache:7");		/* +0 */
	/* Write back the bytes that are already at offset 0: the content is
	 * unchanged, so every later expectation still holds. */
	(void)vfs_seek(&b, 0, ZFS_SEEK_SET, (uint32_t *)0);
	if (vfs_write(&b, m9i_ref, 4u) != 0) {
		m9i_fail(ZOS_M9IOFAT_FAIL_WRITE);
		return -1;
	}
	(void)vfs_seek(&b, 0, ZFS_SEEK_SET, (uint32_t *)0);
	if (vfs_read(&b, m9i_buf, 16u) != 16 ||
	    m9i_diff(m9i_buf, m9i_ref, 16u) != 0u) {
		m9i_fail(ZOS_M9IOFAT_FAIL_COMPARE);
		return -1;
	}
	m9i_note("cache:8");		/* +1: a confirmed write drops it */

	/* The other open file's confirmed write drops it too. */
	if (vfs_open(M9I_B, (uint16_t)(ZFS_O_WRITE | ZFS_O_CREATE |
				       ZFS_O_TRUNC), &a) != 0 ||
	    vfs_write(&a, "BBBB", 4u) != 0) {
		m9i_fail(ZOS_M9IOFAT_FAIL_WRITE);
		return -1;
	}
	(void)vfs_close(&a);
	(void)vfs_seek(&b, 0, ZFS_SEEK_SET, (uint32_t *)0);
	if (vfs_read(&b, m9i_buf, 16u) != 16 ||
	    m9i_diff(m9i_buf, m9i_ref, 16u) != 0u) {
		m9i_fail(ZOS_M9IOFAT_FAIL_COMPARE);
		return -1;
	}
	m9i_note("cache:9");		/* +1: the other file invalidated it */
	if (vfs_read(&b, m9i_buf, 16u) != 16 ||
	    m9i_diff(m9i_buf, &m9i_ref[16], 16u) != 0u) {
		m9i_fail(ZOS_M9IOFAT_FAIL_COMPARE);
		return -1;
	}
	m9i_note("cache:10");		/* +0 */
	(void)vfs_close(&b);

	/* A media change reports STALE, drops the slot, and the next read
	 * fetches again.  The simulator arms the fault on the next note, and
	 * the read must target a block that is not resident so a real FS2
	 * command is sent. */
	if (vfs_open(M9I_A, ZFS_O_READ, &a) != 0) {
		m9i_fail(ZOS_M9IOFAT_FAIL_OPEN);
		return -1;
	}
	if (vfs_read(&a, m9i_buf, 16u) != 16 ||
	    m9i_diff(m9i_buf, m9i_ref, 16u) != 0u) {
		m9i_fail(ZOS_M9IOFAT_FAIL_COMPARE);
		return -1;
	}
	m9i_note("cache:11");		/* +1: block 0 under the new handle */
	/* A media change reports STALE.  Without one the read simply fetches
	 * the block; both outcomes are consistent, and the simulator replay
	 * arms the fault and asserts the branch it configured. */
	m9i_note("cache:stale:arm");
	(void)vfs_seek(&a, 1024, ZFS_SEEK_SET, (uint32_t *)0);
	result = vfs_read(&a, m9i_buf, 16u);
	if (result == -ZOS_ESTALE) {
		m9i_note("cache:stale:observed");
	} else if (result == 16 &&
		   m9i_slice_diff(m9i_buf, 1024u, 16u) == 0u) {
		m9i_note("cache:stale:fresh");
	} else {
		m9i_fail(ZOS_M9IOFAT_FAIL_STALE);
		return -1;
	}
	m9i_note("cache:stale:end");	/* +1 READ either way */
	(void)vfs_seek(&a, 0, ZFS_SEEK_SET, (uint32_t *)0);
	if (vfs_read(&a, m9i_buf, 16u) != 16 ||
	    m9i_diff(m9i_buf, m9i_ref, 16u) != 0u) {
		m9i_fail(ZOS_M9IOFAT_FAIL_COMPARE);
		return -1;
	}
	m9i_note("cache:12");		/* +1: the STALE dropped the slot */
	(void)vfs_close(&a);

	/* Close releases the slot with the handle. */
	if (vfs_open(M9I_A, ZFS_O_READ, &a) != 0) {
		m9i_fail(ZOS_M9IOFAT_FAIL_OPEN);
		return -1;
	}
	if (vfs_read(&a, m9i_buf, 16u) != 16 ||
	    m9i_diff(m9i_buf, m9i_ref, 16u) != 0u) {
		m9i_fail(ZOS_M9IOFAT_FAIL_COMPARE);
		return -1;
	}
	m9i_note("cache:13");		/* +1 */
	(void)vfs_close(&a);
	if (vfs_unlink(M9I_A) != 0 || vfs_unlink(M9I_B) != 0) {
		m9i_fail(ZOS_M9IOFAT_FAIL_UNLINK);
		return -1;
	}
	return 0;
}

/* A card whose CAPS report READ_ONLY: every mutation is refused and the
 * read path still works.  The fixture file is optional, so this branch also
 * covers an operator's write-protected card. */
static int m9i_case_readonly(void)
{
	struct vfs_file file;

	if (vfs_stat(M9I_RO, &m9i_stat) == 0) {
		if (m9i_stat.type != ZFS_TYPE_FILE ||
		    m9i_stat.size == 0u) {
			m9i_fail(ZOS_M9IOFAT_FAIL_STAT);
			return -1;
		}
		if (vfs_open(M9I_RO, ZFS_O_READ, &file) != 0) {
			m9i_fail(ZOS_M9IOFAT_FAIL_OPEN);
			return -1;
		}
		if (vfs_read(&file, m9i_buf, (uint16_t)m9i_stat.size) !=
		    (int)m9i_stat.size) {
			(void)vfs_close(&file);
			m9i_fail(ZOS_M9IOFAT_FAIL_READ);
			return -1;
		}
		(void)vfs_close(&file);
	} else {
		m9i_note("read-only card: no fixture file");
	}
	if (vfs_opendir("/", &m9i_dir) != 0) {
		m9i_fail(ZOS_M9IOFAT_FAIL_READDIR);
		return -1;
	}
	(void)vfs_closedir(&m9i_dir);

	if (vfs_open(M9I_D1, (uint16_t)(ZFS_O_WRITE | ZFS_O_CREATE), &file) !=
		    -ZOS_EROFS ||
	    vfs_unlink(M9I_RO) != -ZOS_EROFS ||
	    vfs_rename(M9I_RO, M9I_B) != -ZOS_EROFS ||
	    vfs_mkdir(M9I_D1) != -ZOS_EROFS ||
	    vfs_rmdir(M9I_ROOT) != -ZOS_EROFS) {
		m9i_fail(ZOS_M9IOFAT_FAIL_EROFS);
		return -1;
	}
	m9i_note("read-only card: EROFS enforced");
	return 0;
}

/* The writable-card surface: open modes, sizes, mutation, enumeration,
 * descriptor exhaustion and the staleness probe. */
static int m9i_case_full(void)
{
	struct vfs_file f1;
	struct vfs_file f2;
	uint8_t seen;
	int result;

	if (vfs_mkdir(M9I_D1) != 0 || vfs_mkdir(M9I_D2) != 0) {
		m9i_fail(ZOS_M9IOFAT_FAIL_MKDIR);
		return -1;
	}

	/* Single-block round trip, size report and stat type. */
	if (m9i_write_file(M9I_A, 300u) != 0) {
		m9i_fail(ZOS_M9IOFAT_FAIL_WRITE);
		return -1;
	}
	if (vfs_stat(M9I_A, &m9i_stat) != 0 ||
	    m9i_stat.type != ZFS_TYPE_FILE || m9i_stat.size != 300u) {
		m9i_fail(ZOS_M9IOFAT_FAIL_STAT);
		return -1;
	}
	if (m9i_read_at(M9I_A, 0u, 300u) != 300 ||
	    m9i_diff(m9i_buf, m9i_ref, 300u) != 0u) {
		m9i_fail(ZOS_M9IOFAT_FAIL_COMPARE);
		return -1;
	}

	/* Cross-block read: 800 bytes, read 300 from offset 400. */
	if (m9i_write_file(M9I_A, 800u) != 0) {
		m9i_fail(ZOS_M9IOFAT_FAIL_WRITE);
		return -1;
	}
	if (m9i_read_at(M9I_A, 400u, 300u) != 300 ||
	    m9i_slice_diff(m9i_buf, 400u, 300u) != 0u) {
		m9i_fail(ZOS_M9IOFAT_FAIL_COMPARE);
		return -1;
	}

	/* A short read at end of file is a successful partial read. */
	if (m9i_read_at(M9I_A, 700u, 300u) != 100) {
		m9i_fail(ZOS_M9IOFAT_FAIL_READ);
		return -1;
	}

	/* A truncating open reports size 0. */
	if (vfs_open(M9I_A, (uint16_t)(ZFS_O_WRITE | ZFS_O_TRUNC), &f1) != 0) {
		m9i_fail(ZOS_M9IOFAT_FAIL_TRUNC);
		return -1;
	}
	if (vfs_close(&f1) != 0 || vfs_stat(M9I_A, &m9i_stat) != 0 ||
	    m9i_stat.size != 0u) {
		m9i_fail(ZOS_M9IOFAT_FAIL_TRUNC);
		return -1;
	}

	/* Opening a directory as a file is EISDIR. */
	if (vfs_open(M9I_D2, ZFS_O_READ, &f1) != -ZOS_EISDIR) {
		m9i_fail(ZOS_M9IOFAT_FAIL_EISDIR);
		return -1;
	}

	/* A same-directory rename moves the name. */
	if (m9i_write_file(M9I_A, 100u) != 0 ||
	    vfs_rename(M9I_A, M9I_B) != 0 ||
	    vfs_stat(M9I_B, &m9i_stat) != 0 || m9i_stat.size != 100u ||
	    vfs_stat(M9I_A, &m9i_stat) != -ZOS_ENOENT) {
		m9i_fail(ZOS_M9IOFAT_FAIL_RENAME);
		return -1;
	}

	/* A cross-directory rename is EXDEV (FS2 RENAME is same-directory). */
	if (m9i_write_file(M9I_C1, 50u) != 0 ||
	    vfs_rename(M9I_C1, M9I_C2) != -ZOS_EXDEV ||
	    vfs_stat(M9I_C1, &m9i_stat) != 0) {
		m9i_fail(ZOS_M9IOFAT_FAIL_EXDEV);
		return -1;
	}

	/* readdir sees the directory entries. */
	if (vfs_opendir(M9I_ROOT, &m9i_dir) != 0) {
		m9i_fail(ZOS_M9IOFAT_FAIL_READDIR);
		return -1;
	}
	seen = 0u;
	for (;;) {
		result = vfs_readdir(&m9i_dir, &m9i_dirent);
		if (result <= 0) {
			break;
		}
		++seen;
	}
	(void)vfs_closedir(&m9i_dir);
	if (result != 0 || seen < 3u) {
		m9i_fail(ZOS_M9IOFAT_FAIL_READDIR);
		return -1;
	}

	/* rmdir of a non-empty directory is ENOTEMPTY. */
	if (vfs_rmdir(M9I_D1) != -ZOS_ENOTEMPTY) {
		m9i_fail(ZOS_M9IOFAT_FAIL_ENOTEMPTY);
		return -1;
	}

	/* Open-handle exhaustion: only two file slots exist. */
	if (m9i_write_file(M9I_A, 4u) != 0 ||
	    vfs_open(M9I_A, ZFS_O_READ, &f1) != 0 ||
	    vfs_open(M9I_A, ZFS_O_READ, &f2) != 0) {
		m9i_fail(ZOS_M9IOFAT_FAIL_EMFILE);
		return -1;
	}
	if (vfs_open(M9I_A, ZFS_O_READ, &m9i_file) != -ZOS_EMFILE) {
		(void)vfs_close(&f1);
		(void)vfs_close(&f2);
		m9i_fail(ZOS_M9IOFAT_FAIL_EMFILE);
		return -1;
	}
	(void)vfs_close(&f1);
	(void)vfs_close(&f2);

	/* Staleness probe: a forced STALE fault makes this read return ESTALE;
	 * without one the file reads back normally.  Both outcomes are
	 * consistent, and the simulator replay asserts the branch it
	 * configured. */
	if (m9i_write_file(M9I_ST, 4u) != 0 ||
	    vfs_open(M9I_ST, ZFS_O_READ, &f1) != 0) {
		m9i_fail(ZOS_M9IOFAT_FAIL_STALE);
		return -1;
	}
	result = vfs_read(&f1, m9i_buf, 4u);
	(void)vfs_close(&f1);
	if (result == -ZOS_ESTALE) {
		m9i_note("stale observed");
	} else if (result == 4 && m9i_diff(m9i_buf, m9i_ref, 4u) == 0u) {
		m9i_note("fresh read");
	} else {
		m9i_fail(ZOS_M9IOFAT_FAIL_STALE);
		return -1;
	}

	/* Clean up: unlink the files and remove every directory we made. */
	if (vfs_unlink(M9I_B) != 0 || vfs_unlink(M9I_A) != 0 ||
	    vfs_unlink(M9I_ST) != 0 || vfs_unlink(M9I_C1) != 0 ||
	    vfs_rmdir(M9I_D1) != 0 || vfs_rmdir(M9I_D2) != 0 ||
	    vfs_rmdir(M9I_ROOT) != 0) {
		m9i_fail(ZOS_M9IOFAT_FAIL_UNLINK);
		return -1;
	}
	return 0;
}

void zos_run_m9_iocfat_selftest(void)
{
	int mounted;
	int result;

	m9_iocfat_selftest_result = ZOS_M9IOFAT_OK;
	mounted = m9i_case_mount();
	if (mounted < 0) {
		return;
	}
	if (mounted > 0) {
		m9i_puts("m9 iocfat selftest: OK (card absent)\r\n");
		return;
	}

	/* `/` is mounted; learn whether the card accepts writes. */
	result = vfs_mkdir(M9I_ROOT);
	if (result == -ZOS_EEXIST) {
		/* A leftover directory is answered by the VFS before the
		 * provider, so probe a name that cannot exist. */
		result = vfs_mkdir(M9I_ROOT "/P");
		if (result == 0) {
			(void)vfs_rmdir(M9I_ROOT "/P");
		}
	}
	if (result == -ZOS_EROFS) {
		if (m9i_case_readonly() != 0) {
			return;
		}
	} else {
		if (result != 0) {
			m9i_fail(ZOS_M9IOFAT_FAIL_MKDIR);
			return;
		}
		if (m9i_case_names() != 0 || m9i_case_cache() != 0 ||
		    m9i_case_full() != 0) {
			return;
		}
		/* The native acceptance program drives the same provider
		 * through the public FS_* family, so the user-visible path
		 * (window copy, descriptor pool, gate stack) is covered by
		 * the same binary that runs on hardware. */
		{
			uint16_t status = 0u;

			if (process_exec("/SYSTEM/fstest.zex", &status) !=
				    ZEX_EXEC_OK || status != 0u) {
				m9i_fail(ZOS_M9IOFAT_FAIL_FSTEST);
				return;
			}
		}
		m9i_note("fstest iocfat section passed");
	}
	m9i_puts("m9 iocfat selftest: OK\r\n");
}

#endif /* ZOS_M9_IOCFAT_SELFTEST */
