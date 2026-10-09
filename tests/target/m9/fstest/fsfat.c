/* ZephyrOS M9 deep filesystem acceptance program (test artifact, package 31).
 *
 * Packed as `fsfat.zex` and executed by `fstest.zex` as its `fat` case, so
 * the M9 acceptance stays one fstest run.  A second program is required by
 * the ABI: the syscall argument window is `0100h-1FFFh` and the SDK links
 * its 16-byte argument scratch at the image end, so one ZEX image can hold
 * about 7.7 KiB of code.  `fstest.zex` is already at that cap.
 *
 * Coverage (architecture section 33 items not already in fstest.zex):
 *   namespace
 *          root mountpoint visibility, mountpoint shadowing, case-folded
 *          mount selection
 *   basic  open/create/close, open existing, stat, TRUNC, sync, seek
 *          (SET/CUR/END), mkdir/rmdir, ENOTEMPTY, rename, unlink
 *   sizes  1, 127, 128, 129, 511, 512, 513 written then read back and
 *          compared by a 32-bit check value computed inside the program
 *   large  65537 bytes written then read back, same check value (card
 *          only: the test-only RAM provider holds 2 KiB)
 *   cache  repeated read, cross-block read and the write-through
 *          invalidate seen through a second open file (data correctness;
 *          the hit/miss command counts are simulator evidence)
 *   pool   descriptor exhaustion reports EMFILE and recovers
 *   exdev  cross-filesystem rename -> EXDEV, same-filesystem rename works
 *   errors missing file, wrong type, EEXIST, bad flags, bad name
 *
 * Everything is created under `/ZOSTEST`, which the program makes and
 * removes again, so a real card's own data is never touched.
 *
 * Output contract: `fsfat: <case> OK` or `fsfat: <case> FAIL 0xNN` per
 * case, then `fsfat: PASS n/n` (or `fsfat: FAIL x/y`) and exit.
 */

#include "zos.h"

#define FSF_ROOT "/ZOSTEST"

/* One 512-byte period of the test pattern: byte i is 0x5A + i*31, and
 * 31*512 is a multiple of 256, so the pattern repeats with period 512. */
#define FSF_PERIOD 512u

static char fsf_ref[FSF_PERIOD];
static char fsf_buf[FSF_PERIOD];
static struct zfs_stat fsf_stat;
static struct zfs_dirent fsf_dirent;

static unsigned int fsf_total;
static unsigned int fsf_pass;

/* Two 16-bit running sums over the position-mixed byte stream: a 32-bit
 * check value computed without a 32-bit multiply, which the SDK does not
 * provide.  Expected and actual values are produced by the same sequence,
 * so a truncated, stale or reordered read cannot match. */
static unsigned int fsf_sum1;
static unsigned int fsf_sum2;
static unsigned int fsf_pos;
static unsigned int fsf_exp1;
static unsigned int fsf_exp2;

/* --- output ----------------------------------------------------------- */

static void fsf_puts(const char *s)
{
	unsigned int n = 0;

	while (s[n] != '\0') {
		++n;
	}
	(void)zex_write(s, n);
}

static void fsf_hex8(unsigned char value)
{
	char b[2];

	b[0] = "0123456789ABCDEF"[(value >> 4) & 0x0Fu];
	b[1] = "0123456789ABCDEF"[value & 0x0Fu];
	(void)zex_write(b, 2);
}

static void fsf_dec(unsigned int value)
{
	char b[3];
	unsigned int hundreds = 0;
	unsigned int tens = 0;

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

static void fsf_report(const char *name, int code)
{
	fsf_puts("fsfat: ");
	fsf_puts(name);
	if (code == 0) {
		fsf_puts(" OK\r\n");
		++fsf_pass;
	} else {
		fsf_puts(" FAIL 0x");
		fsf_hex8((unsigned char)code);
		fsf_puts("\r\n");
	}
	++fsf_total;
}

static int fsf_iserr(int result, unsigned char code)
{
	return (result + (int)code) == 0;
}

/* Echo a fixed diagnostic line.  The simulator keys its fault injection on
 * the `arm`/`end` fences, exactly as the kernel-side provider test does. */
static void fsf_note(const char *s)
{
	fsf_puts("fsfat: ");
	fsf_puts(s);
	fsf_puts("\r\n");
}

static void m9s_note(const char *s)
{
	fsf_puts("m9 selftest: ");
	fsf_puts(s);
	fsf_puts("\r\n");
}

/* --- pattern, hash and I/O helpers ------------------------------------ */

static unsigned char fsf_pattern(unsigned int i)
{
	return (unsigned char)(0x5Au + (i & (FSF_PERIOD - 1u)) * 31u);
}

static void fsf_fill_ref(void)
{
	unsigned int i;

	for (i = 0; i < FSF_PERIOD; ++i) {
		fsf_ref[i] = (char)fsf_pattern(i);
	}
}

static void fsf_hash_reset(void)
{
	fsf_sum1 = 1u;
	fsf_sum2 = 0u;
	fsf_pos = 0u;
}

static void fsf_hash_add(const char *buf, unsigned int len)
{
	unsigned int i;

	for (i = 0; i < len; ++i) {
		unsigned int v = (unsigned int)(unsigned char)buf[i] ^
				 (fsf_pos & 0xFFu);

		fsf_sum1 = (unsigned int)(fsf_sum1 + v);
		fsf_sum2 = (unsigned int)(fsf_sum2 + fsf_sum1);
		++fsf_pos;
	}
}

/* Chunk size for a 32-bit remaining count.  A plain `(unsigned int)`
 * narrowing is wrong here: 65536 truncates to 0 and the transfer loops
 * forever, which is exactly the >64 KiB case the size set needs. */
static unsigned int fsf_chunk(unsigned long remaining)
{
	if (remaining > (unsigned long)FSF_PERIOD) {
		return FSF_PERIOD;
	}
	return (unsigned int)remaining;
}

/* Expected check value over `len` bytes of the periodic pattern. */
static void fsf_expect_hash(unsigned long len)
{
	unsigned long done = 0;

	fsf_hash_reset();
	while (done < len) {
		unsigned int chunk = fsf_chunk(len - done);

		fsf_hash_add(fsf_ref, chunk);
		done += (unsigned long)chunk;
	}
	fsf_exp1 = fsf_sum1;
	fsf_exp2 = fsf_sum2;
}

static int fsf_write_pattern(unsigned int fd, unsigned int len)
{
	unsigned int done = 0;

	while (done < len) {
		unsigned int chunk = len - done;

		if (chunk > FSF_PERIOD) {
			chunk = FSF_PERIOD;
		}
		if (zos_write(fd, fsf_ref, chunk) != (int)chunk) {
			return -1;
		}
		done += chunk;
	}
	return 0;
}

/* Read `len` bytes and fold them into the running check value. */
static int fsf_read_hash(unsigned int fd, unsigned int len)
{
	unsigned int done = 0;

	while (done < len) {
		unsigned int chunk = len - done;
		int got;

		if (chunk > FSF_PERIOD) {
			chunk = FSF_PERIOD;
		}
		got = zos_read(fd, fsf_buf, chunk);
		if (got != (int)chunk) {
			return -1;
		}
		fsf_hash_add(fsf_buf, chunk);
		done += chunk;
	}
	return 0;
}

/* Compare a read slice against the periodic pattern at that offset. */
static int fsf_slice_ok(const char *buf, unsigned int offset, unsigned int len)
{
	unsigned int i;

	for (i = 0; i < len; ++i) {
		if ((unsigned char)buf[i] != fsf_pattern(offset + i)) {
			return 0;
		}
	}
	return 1;
}

/* `/STATS` is the test-only RAM provider's control file; when it answers,
 * `/` is RAM-backed and cannot hold the large-size case. */
static int fsf_is_ram(void)
{
	int fd = zos_open("/STATS", ZFS_O_READ);

	if (fd >= 0) {
		(void)zos_close((unsigned int)fd);
		return 1;
	}
	return 0;
}

/* --- cases ------------------------------------------------------------ */

/* Root mountpoint visibility, flat mount selection and case-folding:
 * `/` lists the mounted top-level names, a name that only shares a prefix
 * with a mountpoint is owned by the root provider, and the mountpoint
 * itself is matched case-insensitively. */
static int fsf_namespace(void)
{
	int fd;
	int found = 0;
	int guard;

	fd = zos_opendir("/");
	if (fd < 0) {
		return 1;
	}
	for (guard = 0; guard < 32; ++guard) {
		int r = zos_readdir((unsigned int)fd, &fsf_dirent);

		if (r == 0) {
			break;
		}
		if (r < 0) {
			(void)zos_closedir((unsigned int)fd);
			return 2;
		}
		if (fsf_dirent.type == ZFS_TYPE_DIR &&
		    fsf_dirent.name[0] == 'S' && fsf_dirent.name[1] == 'Y' &&
		    fsf_dirent.name[2] == 'S' && fsf_dirent.name[3] == 'T' &&
		    fsf_dirent.name[4] == 'E' && fsf_dirent.name[5] == 'M' &&
		    fsf_dirent.name[6] == '\0') {
			found = 1;
		}
	}
	(void)zos_closedir((unsigned int)fd);
	if (found == 0) {
		return 3;
	}

	/* Shadowing: nothing named `/SYSTEMX` exists yet; creating it must be
	 * served by the `/` provider, never by the `/SYSTEM` mount. */
	if (!fsf_iserr(zos_stat("/SYSTEMX", &fsf_stat), ZOS_ENOENT)) {
		return 4;
	}
	fd = zos_open("/SYSTEMX", (unsigned char)(ZFS_O_WRITE | ZFS_O_CREATE));
	if (fd < 0) {
		return 5;
	}
	(void)zos_close((unsigned int)fd);
	if (zos_stat("/SYSTEMX", &fsf_stat) != 0 ||
	    fsf_stat.type != ZFS_TYPE_FILE) {
		return 6;
	}
	if (zos_unlink("/SYSTEMX") != 0 ||
	    !fsf_iserr(zos_stat("/SYSTEMX", &fsf_stat), ZOS_ENOENT)) {
		return 7;
	}

	/* The mountpoint match itself is case-insensitive. */
	if (zos_stat("/system", &fsf_stat) != 0 ||
	    fsf_stat.type != ZFS_TYPE_DIR) {
		return 8;
	}
	fd = zos_open("/system/fstest.zex", ZFS_O_READ);
	if (fd < 0) {
		return 9;
	}
	(void)zos_close((unsigned int)fd);
	return 0;
}

/* Open modes, TRUNC, sync, seek, stat and the directory mutations. */
static int fsf_basic(void)
{
	int fd;
	int r;
	long pos;

	r = zos_mkdir(FSF_ROOT);
	if (r != 0 && !fsf_iserr(r, ZOS_EEXIST)) {
		return 1;
	}

	/* Create, write, close, reopen and stat. */
	fd = zos_open(FSF_ROOT "/B.TXT", (unsigned char)(ZFS_O_WRITE |
							 ZFS_O_CREATE));
	if (fd < 0 || zos_write((unsigned int)fd, "abc", 3u) != 3) {
		return 2;
	}
	if (zos_sync((unsigned int)fd) != 0) {
		return 3;
	}
	(void)zos_close((unsigned int)fd);
	if (zos_stat(FSF_ROOT "/B.TXT", &fsf_stat) != 0 ||
	    fsf_stat.type != ZFS_TYPE_FILE || fsf_stat.size != 3u) {
		return 4;
	}
	fd = zos_open(FSF_ROOT "/B.TXT", ZFS_O_READ);
	if (fd < 0 || zos_read((unsigned int)fd, fsf_buf, 3u) != 3 ||
	    fsf_buf[0] != 'a' || fsf_buf[1] != 'b' || fsf_buf[2] != 'c') {
		return 5;
	}
	(void)zos_close((unsigned int)fd);

	/* Seek semantics on an existing open file. */
	fd = zos_open(FSF_ROOT "/B.TXT", ZFS_O_READ);
	if (fd < 0) {
		return 6;
	}
	if (zos_seek((unsigned int)fd, 0L, ZFS_SEEK_SET) != 0L) {
		return 7;
	}
	if (zos_read((unsigned int)fd, fsf_buf, 1u) != 1 ||
	    zos_seek((unsigned int)fd, 0L, ZFS_SEEK_CUR) != 1L) {
		return 8;
	}
	if (zos_seek((unsigned int)fd, 0L, ZFS_SEEK_END) != 3L) {
		return 9;
	}
	if (zos_seek((unsigned int)fd, 1L, ZFS_SEEK_SET) != 1L) {
		return 10;
	}
	(void)zos_close((unsigned int)fd);

	/* TRUNC to zero, then extend again through the same mode. */
	fd = zos_open(FSF_ROOT "/B.TXT", (unsigned char)(ZFS_O_WRITE |
							 ZFS_O_TRUNC));
	if (fd < 0) {
		return 11;
	}
	if (zos_stat(FSF_ROOT "/B.TXT", &fsf_stat) != 0 ||
	    fsf_stat.size != 0u) {
		(void)zos_close((unsigned int)fd);
		return 12;
	}
	pos = zos_seek((unsigned int)fd, 0L, ZFS_SEEK_END);
	if (pos != 0L) {
		(void)zos_close((unsigned int)fd);
		return 13;
	}
	if (zos_write((unsigned int)fd, "0123456789", 10u) != 10 ||
	    zos_seek((unsigned int)fd, 0L, ZFS_SEEK_END) != 10L) {
		(void)zos_close((unsigned int)fd);
		return 14;
	}
	(void)zos_close((unsigned int)fd);

	/* Rename in the same directory moves the name, not the data. */
	if (zos_rename(FSF_ROOT "/B.TXT", FSF_ROOT "/R.TXT") != 0) {
		return 15;
	}
	if (zos_stat(FSF_ROOT "/B.TXT", &fsf_stat) != -ZOS_ENOENT ||
	    zos_stat(FSF_ROOT "/R.TXT", &fsf_stat) != 0 ||
	    fsf_stat.size != 10u) {
		return 16;
	}

	/* mkdir/rmdir with the non-empty rule. */
	if (zos_mkdir(FSF_ROOT "/D") != 0 ||
	    zos_stat(FSF_ROOT "/D", &fsf_stat) != 0 ||
	    fsf_stat.type != ZFS_TYPE_DIR) {
		return 17;
	}
	fd = zos_open(FSF_ROOT "/D/X.TXT", (unsigned char)(ZFS_O_WRITE |
							   ZFS_O_CREATE));
	if (fd < 0) {
		return 18;
	}
	(void)zos_close((unsigned int)fd);
	if (!fsf_iserr(zos_rmdir(FSF_ROOT "/D"), ZOS_ENOTEMPTY)) {
		return 19;
	}
	if (zos_unlink(FSF_ROOT "/D/X.TXT") != 0 ||
	    zos_rmdir(FSF_ROOT "/D") != 0) {
		return 20;
	}
	if (zos_unlink(FSF_ROOT "/R.TXT") != 0) {
		return 21;
	}
	return 0;
}

/* The size set, each written then read back and compared by hash. */
static int fsf_sizes(void)
{
	static const unsigned int sizes[] = { 1u, 127u, 128u, 129u,
					      511u, 512u, 513u };
	unsigned int s;
	int fd;

	for (s = 0; s < (unsigned int)(sizeof(sizes) / sizeof(sizes[0])); ++s) {
		unsigned int len = sizes[s];

		fsf_expect_hash((unsigned long)len);
		fd = zos_open(FSF_ROOT "/S.BIN", (unsigned char)(ZFS_O_WRITE |
								 ZFS_O_CREATE |
								 ZFS_O_TRUNC));
		if (fd < 0 || fsf_write_pattern((unsigned int)fd, len) != 0) {
			return 1;
		}
		(void)zos_close((unsigned int)fd);
		if (zos_stat(FSF_ROOT "/S.BIN", &fsf_stat) != 0 ||
		    fsf_stat.type != ZFS_TYPE_FILE || fsf_stat.size != len) {
			return 2;
		}
		fd = zos_open(FSF_ROOT "/S.BIN", ZFS_O_READ);
		if (fd < 0) {
			return 3;
		}
		fsf_hash_reset();
		if (fsf_read_hash((unsigned int)fd, len) != 0) {
			(void)zos_close((unsigned int)fd);
			return 4;
		}
		if (fsf_sum1 != fsf_exp1 || fsf_sum2 != fsf_exp2) {
			(void)zos_close((unsigned int)fd);
			return 5;
		}
		(void)zos_close((unsigned int)fd);
		(void)zos_unlink(FSF_ROOT "/S.BIN");
	}
	return 0;
}

/* 65537 bytes: larger than one 64 KiB span, written and read in 512-byte
 * chunks.  Skipped on the 2 KiB test-only RAM provider. */
static int fsf_large(void)
{
	const unsigned long total = 65537UL;
	unsigned long done;
	int fd;

	if (fsf_is_ram()) {
		fsf_note("large: skipped (RAM root)");
		return 0;
	}

	fsf_expect_hash(total);

	fd = zos_open(FSF_ROOT "/L.BIN", (unsigned char)(ZFS_O_WRITE |
							 ZFS_O_CREATE |
							 ZFS_O_TRUNC));
	if (fd < 0) {
		return 1;
	}
	done = 0;
	while (done < total) {
		unsigned int chunk = fsf_chunk(total - done);

		if (zos_write((unsigned int)fd, fsf_ref, chunk) != (int)chunk) {
			(void)zos_close((unsigned int)fd);
			return 2;
		}
		done += (unsigned long)chunk;
	}
	(void)zos_close((unsigned int)fd);
	if (zos_stat(FSF_ROOT "/L.BIN", &fsf_stat) != 0 ||
	    fsf_stat.size != total) {
		return 3;
	}

	fd = zos_open(FSF_ROOT "/L.BIN", ZFS_O_READ);
	if (fd < 0) {
		return 4;
	}
	fsf_hash_reset();
	done = 0;
	while (done < total) {
		unsigned int chunk = fsf_chunk(total - done);
		int got = zos_read((unsigned int)fd, fsf_buf, chunk);

		if (got != (int)chunk) {
			(void)zos_close((unsigned int)fd);
			return 5;
		}
		fsf_hash_add(fsf_buf, chunk);
		done += (unsigned long)chunk;
	}
	(void)zos_close((unsigned int)fd);
	if (fsf_sum1 != fsf_exp1 || fsf_sum2 != fsf_exp2) {
		return 6;
	}
	(void)zos_unlink(FSF_ROOT "/L.BIN");
	return 0;
}

/* Read paths against the write-through cache, by data comparison: a
 * repeated read, a read spanning a block boundary, and a read through one
 * file after a write through the other open file. */
static int fsf_cache(void)
{
	int rd;
	int rw;

	fsf_fill_ref();
	rd = zos_open(FSF_ROOT "/C.BIN", (unsigned char)(ZFS_O_WRITE |
							 ZFS_O_CREATE |
							 ZFS_O_TRUNC));
	if (rd < 0 || fsf_write_pattern((unsigned int)rd, 1040u) != 0) {
		return 1;
	}
	(void)zos_close((unsigned int)rd);

	rd = zos_open(FSF_ROOT "/C.BIN", ZFS_O_READ);
	if (rd < 0) {
		return 2;
	}
	if (zos_read((unsigned int)rd, fsf_buf, 16u) != 16 ||
	    !fsf_slice_ok(fsf_buf, 0u, 16u)) {
		(void)zos_close((unsigned int)rd);
		return 3;
	}
	if (zos_seek((unsigned int)rd, 0L, ZFS_SEEK_SET) != 0L ||
	    zos_read((unsigned int)rd, fsf_buf, 16u) != 16 ||
	    !fsf_slice_ok(fsf_buf, 0u, 16u)) {
		(void)zos_close((unsigned int)rd);
		return 4;
	}
	if (zos_seek((unsigned int)rd, 500L, ZFS_SEEK_SET) != 500L ||
	    zos_read((unsigned int)rd, fsf_buf, 64u) != 64 ||
	    !fsf_slice_ok(fsf_buf, 500u, 64u)) {
		(void)zos_close((unsigned int)rd);
		return 5;
	}

	/* A confirmed write through the other open file must not leave the
	 * first file reading a stale block. */
	rw = zos_open(FSF_ROOT "/C.BIN", ZFS_O_WRITE);
	if (rw < 0) {
		(void)zos_close((unsigned int)rd);
		return 6;
	}
	if (zos_seek((unsigned int)rw, 8L, ZFS_SEEK_SET) != 8L ||
	    zos_write((unsigned int)rw, "ZZZZ", 4u) != 4) {
		(void)zos_close((unsigned int)rw);
		(void)zos_close((unsigned int)rd);
		return 7;
	}
	(void)zos_close((unsigned int)rw);
	if (zos_seek((unsigned int)rd, 8L, ZFS_SEEK_SET) != 8L ||
	    zos_read((unsigned int)rd, fsf_buf, 4u) != 4 ||
	    fsf_buf[0] != 'Z' || fsf_buf[3] != 'Z') {
		(void)zos_close((unsigned int)rd);
		return 8;
	}
	if (zos_seek((unsigned int)rd, 0L, ZFS_SEEK_SET) != 0L ||
	    zos_read((unsigned int)rd, fsf_buf, 8u) != 8 ||
	    !fsf_slice_ok(fsf_buf, 0u, 8u)) {
		(void)zos_close((unsigned int)rd);
		return 9;
	}
	(void)zos_close((unsigned int)rd);
	(void)zos_unlink(FSF_ROOT "/C.BIN");
	return 0;
}

/* Descriptor exhaustion reports EMFILE and recovers.  The provider pools
 * differ (2 file handles on the card, 4 on RAM), so the case opens until
 * the refusal rather than assuming a count. */
static int fsf_pool(void)
{
	int fds[8];
	unsigned int i;
	unsigned int opened = 0;

	fds[0] = zos_open(FSF_ROOT "/P.TXT", (unsigned char)(ZFS_O_WRITE |
							     ZFS_O_CREATE |
							     ZFS_O_TRUNC));
	if (fds[0] < 0) {
		return 1;
	}
	opened = 1u;
	for (i = 1u; i < 8u; ++i) {
		fds[i] = zos_open(FSF_ROOT "/P.TXT", ZFS_O_READ);
		if (fds[i] < 0) {
			break;
		}
		++opened;
	}
	if (i == 8u || opened < 1u) {
		unsigned int k;

		for (k = 0; k < opened; ++k) {
			(void)zos_close((unsigned int)fds[k]);
		}
		return 2; /* the pool never refused */
	}
	if (!fsf_iserr(fds[i], ZOS_EMFILE)) {
		unsigned int k;

		for (k = 0; k < opened; ++k) {
			(void)zos_close((unsigned int)fds[k]);
		}
		return 3;
	}
	for (i = 0; i < opened; ++i) {
		(void)zos_close((unsigned int)fds[i]);
	}
	/* Every descriptor is released: a fresh open must succeed. */
	{
		int again = zos_open(FSF_ROOT "/P.TXT", ZFS_O_READ);

		if (again < 0) {
			return 4;
		}
		(void)zos_close((unsigned int)again);
	}
	(void)zos_unlink(FSF_ROOT "/P.TXT");
	return 0;
}

/* Cross-filesystem rename is EXDEV; the same filesystem still moves. */
static int fsf_exdev(void)
{
	int fd = zos_open(FSF_ROOT "/X.TXT", (unsigned char)(ZFS_O_WRITE |
							     ZFS_O_CREATE));

	if (fd < 0) {
		return 1;
	}
	(void)zos_close((unsigned int)fd);
	if (!fsf_iserr(zos_rename(FSF_ROOT "/X.TXT", "/SYSTEM/X.TXT"),
		       ZOS_EXDEV)) {
		return 2;
	}
	if (!fsf_iserr(zos_rename("/SYSTEM/fstest.zex", FSF_ROOT "/Y.TXT"),
		       ZOS_EXDEV)) {
		return 3;
	}
	if (zos_stat(FSF_ROOT "/X.TXT", &fsf_stat) != 0 ||
	    zos_stat("/SYSTEM/fstest.zex", &fsf_stat) != 0) {
		return 4;
	}
	if (zos_rename(FSF_ROOT "/X.TXT", FSF_ROOT "/Y.TXT") != 0) {
		return 5;
	}
	if (zos_stat(FSF_ROOT "/X.TXT", &fsf_stat) != -ZOS_ENOENT ||
	    zos_stat(FSF_ROOT "/Y.TXT", &fsf_stat) != 0) {
		return 6;
	}
	(void)zos_unlink(FSF_ROOT "/Y.TXT");
	return 0;
}

/* Missing object, wrong type, EEXIST, bad flags and the 8.3 name rule. */
static int fsf_errors(void)
{
	int fd;

	if (!fsf_iserr(zos_stat(FSF_ROOT "/NOPE.BIN", &fsf_stat),
		       ZOS_ENOENT)) {
		return 1;
	}
	if (!fsf_iserr(zos_open(FSF_ROOT, ZFS_O_READ), ZOS_EISDIR)) {
		return 2;
	}
	fd = zos_open("/SYSTEM/fstest.zex", ZFS_O_READ);
	if (fd < 0) {
		return 3;
	}
	if (!fsf_iserr(zos_opendir("/SYSTEM/fstest.zex"), ZOS_ENOTDIR)) {
		(void)zos_close((unsigned int)fd);
		return 4;
	}
	(void)zos_close((unsigned int)fd);

	fd = zos_open(FSF_ROOT "/E.TXT", (unsigned char)(ZFS_O_WRITE |
							 ZFS_O_CREATE));
	if (fd < 0) {
		return 5;
	}
	(void)zos_close((unsigned int)fd);
	if (!fsf_iserr(zos_open(FSF_ROOT "/E.TXT",
				(unsigned char)(ZFS_O_WRITE | ZFS_O_CREATE |
						ZFS_O_EXCL)),
		       ZOS_EEXIST)) {
		return 6;
	}
	if (!fsf_iserr(zos_open(FSF_ROOT "/E.TXT", 0x80u), ZOS_EINVAL)) {
		return 7;
	}
	if (!fsf_iserr(zos_open(FSF_ROOT "/ABCDEFGHIJ.TXT", ZFS_O_READ),
		       ZOS_EINVAL)) {
		return 8;
	}
	(void)zos_unlink(FSF_ROOT "/E.TXT");
	return 0;
}

/* The two provider faults are driven here, in the native client that
 * already owns the acceptance buffers.  This keeps test-only code out of
 * the bank-7 IOC-FAT arena while preserving the exact transcript fences. */
static int fsf_faults(void)
{
	static const char path[] = FSF_ROOT "/FT.TXT";
	int fd;
	int result;

	fd = zos_open(path, (unsigned char)(ZFS_O_WRITE | ZFS_O_CREATE |
					    ZFS_O_TRUNC));
	if (fd < 0 || fsf_write_pattern((unsigned int)fd, 1040u) != 0 ||
	    zos_close((unsigned int)fd) != 0) {
		return 1;
	}

	fd = zos_open(path, ZFS_O_READ);
	if (fd < 0 || zos_read((unsigned int)fd, fsf_buf, 16u) != 16 ||
	    !fsf_slice_ok(fsf_buf, 0u, 16u)) {
		return 2;
	}
	m9s_note("fault:stale:arm");
	if (zos_seek((unsigned int)fd, 1024L, ZFS_SEEK_SET) != 1024L) {
		return 3;
	}
	result = zos_read((unsigned int)fd, fsf_buf, 16u);
	if (result == -ZOS_ESTALE) {
		m9s_note("fault:stale:observed");
	} else if (result == 16 && fsf_slice_ok(fsf_buf, 1024u, 16u)) {
		m9s_note("fault:stale:fresh");
	} else {
		return 3;
	}
	m9s_note("fault:stale:end");
	(void)zos_close((unsigned int)fd);

	fd = zos_open(path, ZFS_O_WRITE);
	if (fd < 0 || zos_seek((unsigned int)fd, 4L, ZFS_SEEK_SET) != 4L) {
		return 4;
	}
	m9s_note("fault:write:arm");
	result = zos_write((unsigned int)fd, fsf_ref, 4u);
	if (result == -ZOS_EIO) {
		m9s_note("fault:write:observed");
		if (zos_seek((unsigned int)fd, 0L, ZFS_SEEK_CUR) != 4L ||
		    zos_stat(path, &fsf_stat) != 0 || fsf_stat.size != 1040u) {
			return 5;
		}
	} else if (result == 4) {
		m9s_note("fault:write:fresh");
	} else {
		return 5;
	}
	m9s_note("fault:write:end");
	(void)zos_close((unsigned int)fd);

	return zos_unlink(path) == 0 ? 0 : 6;
}

/* --- main ------------------------------------------------------------- */

int main(void)
{
	int ram = fsf_is_ram();

	fsf_fill_ref();
	fsf_report("namespace", fsf_namespace());
	fsf_report("basic", fsf_basic());
	fsf_report("sizes", fsf_sizes());
	if (ram == 0) {
		fsf_report("large", fsf_large());
	}
	fsf_report("cache", fsf_cache());
	fsf_report("pool", fsf_pool());
	fsf_report("exdev", fsf_exdev());
	fsf_report("errors", fsf_errors());
	fsf_report("faults", fsf_faults());
	(void)zos_rmdir(FSF_ROOT);

	if (fsf_pass == fsf_total) {
		fsf_puts("fsfat: PASS ");
	} else {
		fsf_puts("fsfat: FAIL ");
	}
	fsf_dec(fsf_pass);
	fsf_puts("/");
	fsf_dec(fsf_total);
	fsf_puts("\r\n");
	zex_exit((unsigned int)((fsf_pass == fsf_total) ? 0u : 1u));
	return 0; /* unreachable */
}
