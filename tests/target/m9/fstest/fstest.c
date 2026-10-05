/* ZephyrOS M9 filesystem syscall acceptance program (test artifact).
 *
 * Packed as fstest.zex and run from /SYSTEM of the M9 FS acceptance image
 * (ROMFS at /SYSTEM, writable ramfs at / and /R2).  It uses only the public
 * native wrappers (user/runtime/zex_fs_runtime.h), never a kernel symbol.
 *
 * Output contract: `fstest: <case> OK` or `fstest: <case> FAIL <code>` per
 * case, then `fstest: PASS n/n` (or `fstest: FAIL x/y`) and exit.  Each
 * case returns 0 on success or a small step code identifying the failure.
 */

#include "zephyros/fs.h"

#include "zex_runtime.h"
#include "zex_fs_runtime.h"

/* Native error values (mirror include/zephyros/errno.h). */
#define FT_EINVAL    0x02
#define FT_ENOENT    0x05
#define FT_EIO       0x0A
#define FT_EEXIST    0x10
#define FT_ENOTDIR   0x11
#define FT_EISDIR    0x12
#define FT_ENOTEMPTY 0x13
#define FT_EROFS     0x14
#define FT_ESTALE    0x15
#define FT_EXDEV     0x16
#define FT_EMFILE    0x17

static char ft_buf[520];
static char ft_ref[520];
static struct zfs_stat ft_stat;
static struct zfs_dirent ft_dirent;
static struct zfs_open_args ft_block;

static unsigned int ft_total;
static unsigned int ft_pass;

/* --- output ----------------------------------------------------------- */

static void ft_puts(const char *s)
{
	unsigned int n = 0;

	while (s[n] != '\0') {
		++n;
	}
	(void)zex_write(s, n);
}

static void ft_hex8(unsigned char value)
{
	char b[2];

	b[0] = "0123456789ABCDEF"[(value >> 4) & 0x0Fu];
	b[1] = "0123456789ABCDEF"[value & 0x0Fu];
	(void)zex_write(b, 2);
}

static void ft_dec(unsigned int value)
{
	char b[3];
	unsigned int tens = 0;
	unsigned int hundreds = 0;

	while (value >= 100u) {
		value -= 100u;
		++hundreds;
	}
	while (value >= 10u) {
		value -= 10u;
		++tens;
	}
	if (hundreds != 0u) {
		b[0] = (char)('0' + hundreds);
		b[1] = (char)('0' + tens);
		b[2] = (char)('0' + value);
		(void)zex_write(b, 3);
	} else if (tens != 0u) {
		b[0] = (char)('0' + tens);
		b[1] = (char)('0' + value);
		(void)zex_write(b, 2);
	} else {
		b[0] = (char)('0' + value);
		(void)zex_write(b, 1);
	}
}

static void ft_report(const char *name, int code)
{
	ft_puts("fstest: ");
	ft_puts(name);
	if (code == 0) {
		ft_puts(" OK\r\n");
		++ft_pass;
	} else {
		ft_puts(" FAIL 0x");
		ft_hex8((unsigned char)code);
		ft_puts("\r\n");
	}
	++ft_total;
}

static int ft_iserr(int result, unsigned char code)
{
	/* result == -code without an emitted NEG: result + code == 0. */
	return (result + (int)code) == 0;
}

static void ft_pattern(char *buf, unsigned int len, unsigned char seed)
{
	unsigned int i;

	for (i = 0; i < len; ++i) {
		buf[i] = (char)(unsigned char)((unsigned int)seed + i * 31u);
	}
}

static int ft_range_equal(const char *a, const char *b, unsigned int len)
{
	unsigned int i;

	for (i = 0; i < len; ++i) {
		if (a[i] != b[i]) {
			return 0;
		}
	}
	return 1;
}

/* --- cases ------------------------------------------------------------ */

/* Filesystem namespace: root, mountpoints, case folding, relative paths,
 * `.`/`..` and CWD crossing between the two mounts. */
static int ft_namespace(void)
{
	char cwd[8];
	int fd;

	if (zos_stat("/", &ft_stat) != 0 || ft_stat.type != ZFS_TYPE_DIR) {
		return 1;
	}
	if (zos_stat("/SYSTEM", &ft_stat) != 0 ||
	    ft_stat.type != ZFS_TYPE_DIR) {
		return 2;
	}
	if (zos_stat("/R2", &ft_stat) != 0 ||
	    ft_stat.type != ZFS_TYPE_DIR) {
		return 3;
	}
	if (zos_stat("/system/FSTEST.ZEX", &ft_stat) != 0 ||
	    ft_stat.type != ZFS_TYPE_FILE) {
		return 4;
	}
	fd = zos_open("/SYSTEM/fstest.zex", ZFS_O_READ);
	if (fd < 0) {
		return 5;
	}
	(void)zos_close((unsigned int)fd);

	if (zos_chdir("/SYSTEM") != 0) {
		return 6;
	}
	if (zos_getcwd(cwd, sizeof(cwd)) != 7) {
		return 7;
	}
	fd = zos_open("fstest.zex", ZFS_O_READ);
	if (fd < 0) {
		return 8;
	}
	(void)zos_close((unsigned int)fd);
	fd = zos_open("./fstest.zex", ZFS_O_READ);
	if (fd < 0) {
		return 9;
	}
	(void)zos_close((unsigned int)fd);
	if (zos_chdir("..") != 0) {
		return 10;
	}
	if (zos_getcwd(cwd, sizeof(cwd)) != 1 || cwd[0] != '/') {
		return 11;
	}
	if (zos_chdir("system") != 0) {
		return 12;
	}
	if (zos_getcwd(cwd, sizeof(cwd)) != 7) {
		return 13;
	}
	if (zos_chdir("/SYSTEM") != 0 || zos_chdir("/R2") != 0) {
		return 14;
	}
	if (zos_getcwd(cwd, sizeof(cwd)) != 3) {
		return 15;
	}
	if (zos_chdir("/") != 0 || zos_getcwd(cwd, sizeof(cwd)) != 1) {
		return 16;
	}
	return 0;
}

/* ROMFS: open/read/stat/enumeration and the read-only rule. */
static int ft_romfs(void)
{
	int fd;
	int count;
	int found = 0;
	unsigned int guard;

	fd = zos_open("/SYSTEM/fstest.zex", ZFS_O_READ);
	if (fd < 0) {
		return 1;
	}
	if (zos_stat("/SYSTEM/fstest.zex", &ft_stat) != 0 ||
	    ft_stat.type != ZFS_TYPE_FILE ||
	    (ft_stat.attributes & ZFS_ATTR_READONLY) == 0 ||
	    ft_stat.size == 0) {
		(void)zos_close((unsigned int)fd);
		return 2;
	}
	count = zos_read((unsigned int)fd, ft_buf, 16);
	if (count <= 0) {
		(void)zos_close((unsigned int)fd);
		return 3;
	}
	if (ft_buf[0] != 'Z' || ft_buf[1] != 'E' || ft_buf[2] != 'X' ||
	    ft_buf[3] != '1') {
		(void)zos_close((unsigned int)fd);
		return 4;
	}
	(void)zos_close((unsigned int)fd);

	fd = zos_opendir("/SYSTEM");
	if (fd < 0) {
		return 5;
	}
	for (guard = 0; guard < 64u; ++guard) {
		int r = zos_readdir((unsigned int)fd, &ft_dirent);

		if (r == 0) {
			break;
		}
		if (r < 0) {
			(void)zos_closedir((unsigned int)fd);
			return 6;
		}
		if (ft_dirent.type == ZFS_TYPE_FILE &&
		    ft_dirent.name[0] == 'f' && ft_dirent.name[1] == 's' &&
		    ft_dirent.name[2] == 't' && ft_dirent.name[3] == 'e' &&
		    ft_dirent.name[4] == 's' && ft_dirent.name[5] == 't' &&
		    ft_dirent.name[6] == '.') {
			found = 1;
		}
	}
	(void)zos_closedir((unsigned int)fd);
	if (!found) {
		return 7;
	}

	if (!ft_iserr(zos_open("/SYSTEM", ZFS_O_READ), FT_EISDIR)) {
		return 8;
	}
	if (!ft_iserr(zos_opendir("/SYSTEM/fstest.zex"), FT_ENOTDIR)) {
		return 9;
	}
	if (!ft_iserr(zos_open("/SYSTEM/NOPE.BIN", ZFS_O_READ), FT_ENOENT)) {
		return 10;
	}
	if (!ft_iserr(zos_mkdir("/SYSTEM/D"), FT_EROFS)) {
		return 11;
	}
	if (!ft_iserr(zos_open("/SYSTEM/fstest.zex", ZFS_O_WRITE),
		      FT_EROFS)) {
		return 12;
	}
	if (!ft_iserr(zos_unlink("/SYSTEM/fstest.zex"), FT_EROFS)) {
		return 13;
	}
	if (!ft_iserr(zos_rmdir("/SYSTEM"), 0x04 /* EBUSY */)) {
		return 14;
	}
	return 0;
}

/* ramfs: mutation, TRUNC, unlink/rmdir semantics and the size set. */
static int ft_ramfs(void)
{
	static const unsigned int sizes[] = { 1u, 127u, 128u, 129u,
					      511u, 512u, 513u };
	unsigned int s;
	int fd;

	if (zos_mkdir("/D1") != 0) {
		return 1;
	}
	if (zos_stat("/D1", &ft_stat) != 0 || ft_stat.type != ZFS_TYPE_DIR) {
		return 2;
	}
	fd = zos_open("/D1/F1.TXT",
		      (unsigned char)(ZFS_O_WRITE | ZFS_O_CREATE |
				      ZFS_O_TRUNC));
	if (fd < 0 || zos_write((unsigned int)fd, "ABC", 3) != 3) {
		return 3;
	}
	(void)zos_close((unsigned int)fd);
	if (zos_stat("/D1/F1.TXT", &ft_stat) != 0 || ft_stat.size != 3u) {
		return 4;
	}

	fd = zos_open("/D1/F1.TXT", ZFS_O_READ);
	if (fd < 0 || zos_read((unsigned int)fd, ft_buf, 3) != 3 ||
	    !ft_range_equal(ft_buf, "ABC", 3)) {
		return 5;
	}
	(void)zos_close((unsigned int)fd);

	fd = zos_open("/D1/F1.TXT", (unsigned char)(ZFS_O_WRITE |
						    ZFS_O_TRUNC));
	if (fd < 0) {
		return 6;
	}
	(void)zos_close((unsigned int)fd);
	if (zos_stat("/D1/F1.TXT", &ft_stat) != 0 || ft_stat.size != 0u) {
		return 7;
	}

	if (zos_rename("/D1/F1.TXT", "/D1/F2.TXT") != 0) {
		return 8;
	}
	if (zos_stat("/D1/F1.TXT", &ft_stat) != -FT_ENOENT ||
	    zos_stat("/D1/F2.TXT", &ft_stat) != 0) {
		return 9;
	}
	if (zos_unlink("/D1/F2.TXT") != 0) {
		return 10;
	}
	if (zos_rmdir("/D1") != 0) {
		return 11;
	}

	if (zos_mkdir("/D2") != 0) {
		return 12;
	}
	fd = zos_open("/D2/X", (unsigned char)(ZFS_O_WRITE | ZFS_O_CREATE));
	if (fd < 0) {
		return 13;
	}
	(void)zos_close((unsigned int)fd);
	if (!ft_iserr(zos_rmdir("/D2"), FT_ENOTEMPTY)) {
		return 14;
	}
	if (zos_unlink("/D2/X") != 0 || zos_rmdir("/D2") != 0) {
		return 15;
	}

	for (s = 0; s < (unsigned int)(sizeof(sizes) / sizeof(sizes[0])); ++s) {
		unsigned int len = sizes[s];

		ft_pattern(ft_ref, len, (unsigned char)(len & 0xFFu));
		fd = zos_open("/S.BIN", (unsigned char)(ZFS_O_WRITE |
							ZFS_O_CREATE |
							ZFS_O_TRUNC));
		if (fd < 0 || zos_write((unsigned int)fd, ft_ref, len) !=
				      (int)len) {
			return 16;
		}
		if (zos_seek((unsigned int)fd, 0, ZFS_SEEK_SET) != 0) {
			return 17;
		}
		if (zos_read((unsigned int)fd, ft_buf, len) != (int)len ||
		    !ft_range_equal(ft_buf, ft_ref, len)) {
			return 18;
		}
		(void)zos_close((unsigned int)fd);
		if (zos_stat("/S.BIN", &ft_stat) != 0 ||
		    ft_stat.size != len) {
			return 19;
		}
	}

	/* Seeking past the end is allowed and reported back. */
	fd = zos_open("/S.BIN", ZFS_O_READ);
	if (fd < 0) {
		return 20;
	}
	if (zos_seek((unsigned int)fd, 70000L, ZFS_SEEK_SET) != 70000L) {
		return 21;
	}
	(void)zos_close((unsigned int)fd);
	(void)zos_unlink("/S.BIN");
	return 0;
}

/* Error paths: missing object, wrong type, EEXIST and bad flags. */
static int ft_errors(void)
{
	int fd;

	if (!ft_iserr(zos_stat("/NOPE.BIN", &ft_stat), FT_ENOENT)) {
		return 1;
	}
	if (!ft_iserr(zos_open("/NOPE.BIN", ZFS_O_READ), FT_ENOENT)) {
		return 2;
	}
	if (!ft_iserr(zos_open("/", ZFS_O_READ), FT_EISDIR)) {
		return 3;
	}
	if (!ft_iserr(zos_opendir("/SYSTEM/fstest.zex"), FT_ENOTDIR)) {
		return 4;
	}
	fd = zos_open("/E.TXT", (unsigned char)(ZFS_O_WRITE | ZFS_O_CREATE));
	if (fd < 0) {
		return 5;
	}
	(void)zos_close((unsigned int)fd);
	if (!ft_iserr(zos_open("/E.TXT", (unsigned char)(ZFS_O_WRITE |
							ZFS_O_CREATE |
							ZFS_O_EXCL)),
		      FT_EEXIST)) {
		return 6;
	}
	if (!ft_iserr(zos_open("/NOPE.BIN", 0x80u), FT_EINVAL)) {
		return 7;
	}
	if (!ft_iserr(zos_read(9u, ft_buf, 1), FT_EINVAL)) {
		return 8; /* out-of-range descriptor */
	}
	(void)zos_unlink("/E.TXT");
	return 0;
}

/* Descriptor-pool exhaustion: the 9th simultaneous open is EMFILE.
 *
 * The provider-open-handle pools are smaller than the VFS pool (romfs 2,
 * ramfs 4 per instance), so exhaustion is reached by combining the three
 * mounted filesystems: 4 handles in `/` (ramfs.0), 2 in `/SYSTEM` (romfs)
 * and 2 in `/R2` (ramfs.1) fill all 8 VFS descriptors, and the 9th open is
 * rejected by the VFS pool before any provider is consulted. */
static int ft_pool(void)
{
	int fds[8];
	unsigned int i;
	int ninth;

	fds[0] = zos_open("/P.BIN", (unsigned char)(ZFS_O_WRITE |
						    ZFS_O_CREATE |
						    ZFS_O_TRUNC));
	fds[1] = zos_open("/P.BIN", ZFS_O_READ);
	fds[2] = zos_open("/P.BIN", ZFS_O_READ);
	fds[3] = zos_open("/P.BIN", ZFS_O_READ);
	fds[4] = zos_open("/SYSTEM/fstest.zex", ZFS_O_READ);
	fds[5] = zos_open("/SYSTEM/fstest.zex", ZFS_O_READ);
	fds[6] = zos_open("/R2/Q.BIN", (unsigned char)(ZFS_O_WRITE |
						       ZFS_O_CREATE |
						       ZFS_O_TRUNC));
	fds[7] = zos_open("/R2/Q.BIN", ZFS_O_READ);
	for (i = 0; i < 8u; ++i) {
		if (fds[i] < 0) {
			/* Could not even fill the pool: report at that step. */
			unsigned int k;

			for (k = 0; k < i; ++k) {
				(void)zos_close((unsigned int)fds[k]);
			}
			return (int)(i + 1u);
		}
	}
	ninth = zos_open("/SYSTEM/fstest.zex", ZFS_O_READ);
	for (i = 0; i < 8u; ++i) {
		(void)zos_close((unsigned int)fds[i]);
	}
	(void)zos_unlink("/P.BIN");
	(void)zos_unlink("/R2/Q.BIN");
	if (!ft_iserr(ninth, FT_EMFILE)) {
		return 9;
	}
	return 0;
}

/* Stale handle through the ramfs /STALE control. */
static int ft_stale(void)
{
	int fd;
	int control;

	fd = zos_open("/A.TXT", (unsigned char)(ZFS_O_WRITE | ZFS_O_CREATE |
						ZFS_O_TRUNC));
	if (fd < 0 || zos_write((unsigned int)fd, "hi", 2) != 2) {
		return 1;
	}
	(void)zos_close((unsigned int)fd);
	fd = zos_open("/A.TXT", ZFS_O_READ);
	if (fd < 0) {
		return 2;
	}
	control = zos_open("/STALE", (unsigned char)(ZFS_O_WRITE |
						     ZFS_O_CREATE));
	if (control < 0) {
		return 3;
	}
	(void)zos_close((unsigned int)control);
	if (!ft_iserr(zos_read((unsigned int)fd, ft_buf, 2), FT_ESTALE)) {
		return 4;
	}
	(void)zos_close((unsigned int)fd);
	(void)zos_unlink("/STALE");
	(void)zos_unlink("/A.TXT");
	return 0;
}

/* A failed confirmed write leaves the offset (seek CUR 0) and size. */
static int ft_writefail(void)
{
	int fd;
	int control;

	fd = zos_open("/W.TXT", (unsigned char)(ZFS_O_WRITE | ZFS_O_CREATE |
						ZFS_O_TRUNC));
	if (fd < 0 || zos_write((unsigned int)fd, "AAAA", 4) != 4) {
		return 1;
	}
	control = zos_open("/FAILWRITE", (unsigned char)(ZFS_O_WRITE |
							 ZFS_O_CREATE));
	if (control < 0) {
		return 2;
	}
	(void)zos_close((unsigned int)control);
	if (!ft_iserr(zos_write((unsigned int)fd, "BBBB", 4), FT_EIO)) {
		return 3;
	}
	if (zos_seek((unsigned int)fd, 0L, ZFS_SEEK_CUR) != 4L) {
		return 4;
	}
	if (zos_stat("/W.TXT", &ft_stat) != 0 || ft_stat.size != 4u) {
		return 5;
	}
	(void)zos_close((unsigned int)fd);
	(void)zos_unlink("/W.TXT");
	(void)zos_unlink("/FAILWRITE");
	return 0;
}

/* Cross-mount rename is EXDEV (VFS never copies). */
static int ft_exdev(void)
{
	int fd = zos_open("/X.TXT", (unsigned char)(ZFS_O_WRITE |
						    ZFS_O_CREATE));

	if (fd < 0) {
		return 1;
	}
	(void)zos_close((unsigned int)fd);
	if (!ft_iserr(zos_rename("/X.TXT", "/SYSTEM/X.TXT"), FT_EXDEV)) {
		return 2;
	}
	if (!ft_iserr(zos_rename("/X.TXT", "/R2/X.TXT"), FT_EXDEV)) {
		return 3;
	}
	if (zos_rename("/X.TXT", "/Y.TXT") != 0) {
		return 4;
	}
	(void)zos_unlink("/Y.TXT");
	return 0;
}

/* Argument-window faults are rejected before any state change. */
static int ft_window(void)
{
	struct zos_fs_call_req req;
	int fd;

	fd = zos_open("/SYSTEM/fstest.zex", ZFS_O_READ);
	if (fd < 0) {
		return 1;
	}
	if (!ft_iserr(zos_read((unsigned int)fd, (void *)0x00F0, 1),
		      FT_EINVAL)) {
		return 2;
	}
	if (!ft_iserr(zos_read((unsigned int)fd, (void *)0xDFFF, 2),
		      FT_EINVAL)) {
		return 3;
	}
	if (!ft_iserr(zos_stat("/SYSTEM/fstest.zex",
			       (struct zfs_stat *)0x00F0),
		      FT_EINVAL)) {
		return 4;
	}
	/* Craft a block whose path pointer leaves the caller window; the
	 * kernel must reject it before reading. */
	ft_block.path = 0x2001u;
	ft_block.length = 1u;
	ft_block.flags = ZFS_O_READ;
	req.number = 0x10u; /* FS_OPEN */
	req.block = &ft_block;
	if (!ft_iserr(zos_fs_call(&req), FT_EINVAL)) {
		return 5;
	}
	ft_block.path = 0x0100u;
	ft_block.length = 0u; /* zero-length path */
	req.block = &ft_block;
	if (!ft_iserr(zos_fs_call(&req), FT_EINVAL)) {
		return 6;
	}
	(void)zos_close((unsigned int)fd);
	return 0;
}

/* --- main ------------------------------------------------------------- */

void ft_main(void)
{
	ft_report("namespace", ft_namespace());
	ft_report("romfs", ft_romfs());
	ft_report("ramfs", ft_ramfs());
	ft_report("errors", ft_errors());
	ft_report("pool", ft_pool());
	ft_report("stale", ft_stale());
	ft_report("writefail", ft_writefail());
	ft_report("exdev", ft_exdev());
	ft_report("window", ft_window());

	if (ft_pass == ft_total) {
		ft_puts("fstest: PASS ");
	} else {
		ft_puts("fstest: FAIL ");
	}
	ft_dec(ft_pass);
	ft_puts("/");
	ft_dec(ft_total);
	ft_puts("\r\n");
	zex_exit((unsigned int)((ft_pass == ft_total) ? 0u : 1u));
}
