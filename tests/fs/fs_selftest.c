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
 *     correction 01 adds the same child copied to the card and executed
 *     *from* `/FS`, so a card executable's path is read by a bank-6
 *     provider on the real machine;
 *   * a card that is absent or read-only skips the card outcomes, keeps
 *     `/SYSTEM` working and still ends with a verdict.
 *
 * The replay reads the lines below; the final line is the verdict.  The
 * image then execs the monitor, so the transcript reaches the production
 * prompt.
 *
 * Work package 45b adds one tail option, `stage`: the same program then
 * runs as the hardware exerciser's files stage - every write confined to
 * the /FS/HWTEST scratch directory the runner creates and removes, the
 * nested exec tagging its child the same way, and a return to the caller
 * instead of the monitor handover.  The boot path above is unchanged, so
 * the fs image's transcript is exactly what it was.
 */

#include "zos.h"

#define FS_DIR   "/FS"
#define FS_FILE  "/FS/SELFTEST.TMP"
#define FS_RENAMED "/FS/RENAMED.TMP"
#define FS_SUBDIR "/FS/SELFTEST.D"
#define SYS_FILE "/SYSTEM/services.man"
#define CHILD    "/SYSTEM/fs_child.zex"

/* The stage tail and the scratch paths it selects (45b). */
#define FS_STAGE_TAIL "stage"
#define HW_DIR    "/FS/HWTEST"
#define HW_FILE   "/FS/HWTEST/SELFTEST.TMP"
#define HW_RENAMED "/FS/HWTEST/RENAMED.TMP"
#define HW_SUBDIR "/FS/HWTEST/SELFTEST.D"

/* The child's copy on the card (correction 01): the program this file
 * copies and then execs *from `/FS`*, which is the one way a card
 * executable is proven on the real machine. */
#define FS_CHILD_PROGRAM "/FS/CHILD.ZEX"
#define HW_CHILD_PROGRAM "/FS/HWTEST/CHILD.ZEX"

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
/* ZEX v2 has no initialized writable data: every static starts zeroed. */
static int card;
/* 1: the exerciser's files stage (45b): scratch paths, no handover. */
static int stage;

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

static int card_write(const char *path, unsigned char flags)
{
	int handle = zos_open(path, flags);

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
	const char *file = stage ? HW_FILE : FS_FILE;
	const char *renamed = stage ? HW_RENAMED : FS_RENAMED;
	const char *subdir = stage ? HW_SUBDIR : FS_SUBDIR;
	const char *dir = stage ? HW_DIR : FS_DIR;
	int handle;

	/* create + truncate */
	handle = card_write(file, (unsigned char)(ZFS_O_WRITE | ZFS_O_CREATE |
						  ZFS_O_TRUNC));
	check("fs-open-create", handle >= 0);
	check("fs-write-blocks", handle >= 0);
	if (handle < 0) {
		return;
	}
	check("fs-close-write", zos_close((unsigned int)handle) == 0);

	check("fs-stat-type",
	      zos_stat(file, &st) == 0 && st.type == ZFS_TYPE_FILE);
	check("fs-stat-size", st.size == FS_SIZE);

	/* read back */
	handle = zos_open(file, ZFS_O_READ);
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
	check("enum-card-file", enum_has(dir, "SELFTEST.TMP", 0));

	/* directory ops */
	check("fs-mkdir", zos_mkdir(subdir) == 0);
	check("fs-mkdir-stat",
	      zos_stat(subdir, &st) == 0 && st.type == ZFS_TYPE_DIR);
	check("enum-card-subdir", enum_has(dir, "SELFTEST.D", 1));
	check("fs-rename", zos_rename(file, renamed) == 0);
	check("fs-rename-stat",
	      zos_stat(renamed, &st) == 0 && st.size == FS_SIZE);
	check("fs-rename-old-gone", zos_stat(file, &st) == -ZOS_ENOENT);
	check("fs-unlink", zos_unlink(renamed) == 0);
	check("fs-rmdir", zos_rmdir(subdir) == 0);
	check("fs-unlink-gone", zos_stat(renamed, &st) == -ZOS_ENOENT);
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

/* Copy the child program from the store to the card, so the exec below
 * runs an image that lives on the medium - the path a bank-6 provider has
 * to read.  Returns 1 when the copy completed. */
static int copy_child_to_card(const char *dest)
{
	unsigned int done = 0u;
	int source = zos_open(CHILD, ZFS_O_READ);
	int target;
	int ok = 1;

	if (source < 0) {
		return 0;
	}
	target = zos_open(dest, (unsigned char)(ZFS_O_WRITE | ZFS_O_CREATE |
						ZFS_O_TRUNC));
	if (target < 0) {
		(void)zos_close((unsigned int)source);
		return 0;
	}
	for (;;) {
		int count = zos_read((unsigned int)source, buf, FS_BLOCK);

		if (count <= 0) {
			ok = (count == 0);
			break;
		}
		if (zos_write((unsigned int)target, buf,
			      (unsigned int)count) != count) {
			ok = 0;
			break;
		}
		done += (unsigned int)count;
	}
	if (ok) {
		ok = (zos_sync((unsigned int)target) == 0);
	}
	(void)zos_close((unsigned int)target);
	(void)zos_close((unsigned int)source);
	if (!ok || done == 0u) {
		(void)zos_unlink(dest);
		return 0;
	}
	return 1;
}

static void exec_case(void)
{
	int status = zos_exec(CHILD, stage ? FS_STAGE_TAIL : "tail-42");

	check("exec-status", status == CHILD_STATUS);

	/* The same child, run from the card: the exec bind hands the path to
	 * the bank-6 filesystem provider, so this row fails if exec stages it
	 * anywhere the provider's window cannot see (correction 01). */
	if (card != 0) {
		const char *copy = stage ? HW_CHILD_PROGRAM
					 : FS_CHILD_PROGRAM;

		check("exec-card-copy", copy_child_to_card(copy));
		status = zos_exec(copy, stage ? FS_STAGE_TAIL : "tail-42");
		check("exec-card-status", status == CHILD_STATUS);
		if (stage != 0) {
			(void)zos_unlink(copy);
		}
	}

	/* The child leaked a card file and a bound `/FS`: if the kernel had
	 * not released them, this open would report -ZOS_EMFILE. */
	if (card != 0) {
		int handle = zos_open(stage ? HW_FILE : FS_FILE,
				      (unsigned char)(ZFS_O_WRITE |
						      ZFS_O_CREATE));

		check("exec-child-handles-released", handle >= 0);
		if (handle >= 0) {
			(void)zos_close((unsigned int)handle);
			(void)zos_unlink(stage ? HW_FILE : FS_FILE);
		}
	}
}

static int same(const char *a, const char *b)
{
	while (*a == *b) {
		if (*a == '\0') {
			return 1;
		}
		++a;
		++b;
	}
	return 0;
}

static unsigned int length_of(const char *text)
{
	unsigned int length = 0u;

	while (text[length] != '\0') {
		++length;
	}
	return length;
}

int main(int argc, char **argv)
{
	struct zos_startup *boot = zos_args();
	const char *tail = (const char *)0;
	int probe;

	/* The stage tail (45b) selects the scratch paths and suppresses the
	 * monitor handover; anything else is refused before a check runs. */
	if (argc == 1 && argv != (char **)0 && same(argv[0], FS_STAGE_TAIL)) {
		stage = 1;
		tail = FS_STAGE_TAIL;
	} else if (argc != 0) {
		zos_puts("fs-selftest: FAIL options\r\n");
		return 2;
	}

	zos_puts("fs-selftest: begin\r\n");
	make_pattern();
	card = 1;

	/* Boot contract (42b): version 1 with the empty boot tail.  Run as
	 * the exerciser's stage, the same line proves the option tail
	 * arrived byte-exact instead. */
	check("startup-version",
	      boot != (struct zos_startup *)0 && boot->version == 1u);
	check("startup-tail",
	      boot != (struct zos_startup *)0 &&
	      (tail == (const char *)0
		       ? boot->length == 0u
		       : (boot->length == (unsigned char)length_of(tail) &&
			  same(boot->tail, tail))));
	/* The single `=` is the whole test: one byte through the standard
	 * handle must come back as 1.  The byte itself is therefore the
	 * glyph that appears on the display immediately before this row -
	 * correction 01 confirmed on the bench that the stray `=` is this
	 * check, not a console fault. */
	check("console-write", zos_console_write("=", 1u) == 1);

	/* A card that is absent or write-protected must leave `/SYSTEM`
	 * usable and the verdict honest: probe with a read-only open, which
	 * reports -ZOS_ENODEV when no card answers and -ZOS_EROFS for a
	 * write-protected one. */
	probe = zos_open(stage ? HW_FILE : FS_FILE, ZFS_O_READ);
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

	/* Production-like boot: hand the console to the monitor.  A stage
	 * run returns to the runner that started it. */
	if (stage == 0) {
		(void)zos_exec("/SYSTEM/monitor.zex", "");
	}
	return (failures == 0u) ? 0 : 1;
}
