/* ZephyrOS filesystem outcome acceptance (work package 42b, correction 01).
 *
 * The ZEX primary the `fs-selftest` image boots: an ordinary SDK program
 * that drives the whole card surface through `fs.h` and prints one line per
 * outcome.  It is new code for the M10 model - one small outcome image,
 * no fault-injection matrix and no kernel-side test hook - and it replaces
 * every M9 acceptance program and image.
 *
 * Outcomes:
 *
 *   * the startup block crt0 receives (version 1, empty boot tail) and a
 *     console write through the standard handles;
 *   * `/FS` through `fs.h`: create + truncate, three writes that cross
 *     block boundaries (512 + 512 + 176 = 1200 bytes), sync, close and the
 *     stat size/type;
 *   * read back: the head, a 300-byte run that crosses a block boundary
 *     inside one call (the driver's chunked read regressed exactly there),
 *     the tail to end of file, a read at end of file, and the three seek
 *     origins;
 *   * enumeration: `/` lists `/SYSTEM` and `/FS`, `/FS` lists the file the
 *     program created;
 *   * directory ops: mkdir, the DIR stat, rename, the renamed stat, the
 *     vanished old name, unlink and rmdir;
 *   * `/SYSTEM`: open, stat and read a recovery-store file;
 *   * nested exec: `/SYSTEM/fs_child.zex` gets the tail, writes through
 *     its own stdout, leaks every handle it opened and reports the exit
 *     status this program compares - the card slots the child leaked must
 *     be reusable immediately, which proves the kernel released them;
 *   * a card that is absent or read-only skips the card outcomes, keeps
 *     `/SYSTEM` working and still ends with a verdict.
 *
 * The replay reads the lines below; the final line is the verdict.  The
 * image then execs the monitor, so the transcript reaches the production
 * prompt.
 */

#include "zos.h"

#define FS_DIR   "/FS"
#define FS_FILE  "/FS/SELFTEST.TMP"
#define FS_RENAMED "/FS/RENAMED.TMP"
#define FS_SUBDIR "/FS/SELFTEST.D"
#define SYS_FILE "/SYSTEM/services.man"
#define CHILD    "/SYSTEM/fs_child.zex"

#define FS_BLOCK 512u
#define FS_SIZE  1200u

/* The child's exit status (tests/fs/fs_child.c). */
#define CHILD_STATUS 42

#define ENUM_PAGE 4u

static unsigned char pattern[FS_BLOCK];
static unsigned char buf[FS_BLOCK];
static struct zfs_stat st;
static struct zos_ns_entry entries[ENUM_PAGE];

static unsigned int checks;
static unsigned int failures;
/* ZEX v1 has no initialized writable data: every static starts zeroed. */
static int card;

static void check(const char *label, int ok)
{
	++checks;
	if (!ok) {
		++failures;
		zos_printf("fs-selftest: FAIL %s\r\n", label);
		return;
	}
	zos_printf("fs-selftest: %s OK\r\n", label);
}

/* One 512-byte period of the test pattern: a slice anywhere is checked
 * against `offset & 511`, so a read that delivers the wrong slice is
 * caught by content, not just by length. */
static void make_pattern(void)
{
	unsigned int i;

	for (i = 0u; i < FS_BLOCK; ++i) {
		pattern[i] = (unsigned char)((i * 29u + 11u) & 0xFFu);
	}
}

static int slice_matches(unsigned int offset, unsigned int length)
{
	unsigned int i;

	for (i = 0u; i < length; ++i) {
		if (buf[i] != pattern[(offset + i) & (FS_BLOCK - 1u)]) {
			return 0;
		}
	}
	return 1;
}

/* Page the entry stream at `path` and report whether `name` appeared as a
 * container or as a leaf. */
static int enum_has(const char *path, const char *name, int container)
{
	unsigned int cookie = 0u;
	unsigned int pages = 0u;

	for (;;) {
		int count = zos_ns_enum(path, &cookie, entries, ENUM_PAGE);
		int index;

		if (count < 0) {
			return 0;
		}
		if (count == 0) {
			return 0;
		}
		for (index = 0; index < count; ++index) {
			int is_container =
				(entries[index].flags &
				 ZOS_NS_ENTRY_CONTAINER) != 0u;

			if (strcmp(entries[index].name, name) == 0 &&
			    is_container == (container != 0)) {
				return 1;
			}
		}
		if (++pages > 8u) {
			return 0;
		}
	}
}

/* --- the card surface ---------------------------------------------------- */

static int card_write(unsigned char flags)
{
	int handle = zos_open(FS_FILE, flags);

	if (handle < 0) {
		return handle;
	}
	if (zos_write((unsigned int)handle, pattern, FS_BLOCK) !=
	    (int)FS_BLOCK) {
		(void)zos_close((unsigned int)handle);
		return -1;
	}
	if (zos_write((unsigned int)handle, pattern, FS_BLOCK) !=
	    (int)FS_BLOCK) {
		(void)zos_close((unsigned int)handle);
		return -1;
	}
	if (zos_write((unsigned int)handle, pattern,
		      (unsigned int)(FS_SIZE - 2u * FS_BLOCK)) !=
	    (int)(FS_SIZE - 2u * FS_BLOCK)) {
		(void)zos_close((unsigned int)handle);
		return -1;
	}
	if (zos_sync((unsigned int)handle) != 0) {
		(void)zos_close((unsigned int)handle);
		return -1;
	}
	return handle;
}

static void card_case(void)
{
	int handle;

	/* create + truncate */
	handle = card_write((unsigned char)(ZFS_O_WRITE | ZFS_O_CREATE |
					    ZFS_O_TRUNC));
	check("fs-open-create", handle >= 0);
	check("fs-write-blocks", handle >= 0);
	if (handle < 0) {
		return;
	}
	check("fs-close-write", zos_close((unsigned int)handle) == 0);

	check("fs-stat-type",
	      zos_stat(FS_FILE, &st) == 0 && st.type == ZFS_TYPE_FILE);
	check("fs-stat-size", st.size == FS_SIZE);

	/* read back */
	handle = zos_open(FS_FILE, ZFS_O_READ);
	check("fs-open-read", handle >= 0);
	if (handle < 0) {
		return;
	}
	check("fs-read-head",
	      zos_read((unsigned int)handle, buf, 128u) == 128 &&
	      slice_matches(0u, 128u));
	check("fs-seek-set",
	      zos_seek((unsigned int)handle, 400, ZFS_SEEK_SET) == 400);
	check("fs-read-cross-block",
	      zos_read((unsigned int)handle, buf, 300u) == 300 &&
	      slice_matches(400u, 300u));
	check("fs-seek-end",
	      zos_seek((unsigned int)handle, 0, ZFS_SEEK_END) ==
	      (long)FS_SIZE);
	check("fs-read-eof",
	      zos_read((unsigned int)handle, buf, 64u) == 0);
	check("fs-seek-cur",
	      zos_seek((unsigned int)handle, -100, ZFS_SEEK_CUR) ==
	      (long)(FS_SIZE - 100u));
	check("fs-read-tail",
	      zos_read((unsigned int)handle, buf, 100u) == 100 &&
	      slice_matches(FS_SIZE - 100u, 100u));
	check("fs-close-read", zos_close((unsigned int)handle) == 0);

	/* enumeration */
	check("enum-root-system", enum_has("/", "SYSTEM", 1));
	check("enum-root-fs", enum_has("/", "FS", 1));
	check("enum-card-file", enum_has(FS_DIR, "SELFTEST.TMP", 0));

	/* directory ops */
	check("fs-mkdir", zos_mkdir(FS_SUBDIR) == 0);
	check("fs-mkdir-stat",
	      zos_stat(FS_SUBDIR, &st) == 0 && st.type == ZFS_TYPE_DIR);
	check("enum-card-subdir", enum_has(FS_DIR, "SELFTEST.D", 1));
	check("fs-rename", zos_rename(FS_FILE, FS_RENAMED) == 0);
	check("fs-rename-stat",
	      zos_stat(FS_RENAMED, &st) == 0 && st.size == FS_SIZE);
	check("fs-rename-old-gone", zos_stat(FS_FILE, &st) == -ZOS_ENOENT);
	check("fs-unlink", zos_unlink(FS_RENAMED) == 0);
	check("fs-rmdir", zos_rmdir(FS_SUBDIR) == 0);
	check("fs-unlink-gone", zos_stat(FS_RENAMED, &st) == -ZOS_ENOENT);
}

/* --- the recovery store and the nested exec ------------------------------ */

static void system_case(void)
{
	int handle;
	unsigned char head[16];

	check("system-stat",
	      zos_stat(SYS_FILE, &st) == 0 && st.type == ZFS_TYPE_FILE &&
	      st.size != 0u);
	handle = zos_open(SYS_FILE, ZFS_O_READ);
	check("system-open", handle >= 0);
	if (handle < 0) {
		return;
	}
	check("system-read",
	      zos_read((unsigned int)handle, head, sizeof(head)) ==
	      (int)sizeof(head));
	check("system-close", zos_close((unsigned int)handle) == 0);
}

static void exec_case(void)
{
	int status = zos_exec(CHILD, "tail-42");

	check("exec-status", status == CHILD_STATUS);

	/* The child leaked a card file and a bound `/FS`: if the kernel had
	 * not released them, this open would report -ZOS_EMFILE. */
	if (card != 0) {
		int handle = zos_open(FS_FILE,
				      (unsigned char)(ZFS_O_WRITE |
						      ZFS_O_CREATE));

		check("exec-child-handles-released", handle >= 0);
		if (handle >= 0) {
			(void)zos_close((unsigned int)handle);
			(void)zos_unlink(FS_FILE);
		}
	}
}

int main(void)
{
	struct zos_startup *boot = zos_args();
	int probe;

	zos_puts("fs-selftest: begin\r\n");
	make_pattern();
	card = 1;

	check("startup-version",
	      boot != (struct zos_startup *)0 && boot->version == 1u);
	check("startup-tail",
	      boot != (struct zos_startup *)0 && boot->length == 0u);
	check("console-write", zos_console_write("=", 1u) == 1);

	/* A card that is absent or write-protected must leave `/SYSTEM`
	 * usable and the verdict honest: probe with a read-only open, which
	 * reports -ZOS_ENODEV when no card answers and -ZOS_EROFS for a
	 * write-protected one. */
	probe = zos_open(FS_FILE, ZFS_O_READ);
	if (probe == -ZOS_ENODEV) {
		card = 0;
		zos_puts("fs-selftest: card absent: /FS not served\r\n");
	} else if (probe == -ZOS_EROFS || probe == -ZOS_EINVAL) {
		card = 0;
		zos_puts("fs-selftest: card not writable\r\n");
	} else if (probe >= 0) {
		(void)zos_close((unsigned int)probe);
	} else if (probe != -ZOS_ENOENT) {
		card = 0;
		zos_puts("fs-selftest: /FS not served\r\n");
	}

	if (card != 0) {
		card_case();
	}
	system_case();
	exec_case();

	zos_printf("fs-selftest: %u checks, %u failures\r\n", checks,
		   failures);
	if (failures == 0u) {
		zos_puts("fs-selftest: PASS\r\n");
	} else {
		zos_puts("fs-selftest: FAIL\r\n");
	}

	/* Production-like boot: hand the console to the monitor. */
	(void)zos_exec("/SYSTEM/monitor.zex", "");
	return (failures == 0u) ? 0 : 1;
}
