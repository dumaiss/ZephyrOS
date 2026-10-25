/* KonaOS filesystem outcome acceptance.
 *
 * The EXE primary the `fs-selftest` image boots: an ordinary SDK program
 * that drives the whole card surface through `fs.h` and prints one line per
 * outcome: one small outcome image, no fault-injection matrix and no
 * kernel-side test hook.
 *
 * Outcomes:
 *
 *   * the startup block crt0 receives (version 1, empty boot tail) and a
 *     console write through the standard handles;
 *   * `/fs` through `fs.h`: create + truncate, three writes that cross
 *     block boundaries (512 + 512 + 176 = 1200 bytes), sync, close and the
 *     stat size/type;
 *   * read back: the head, a 300-byte run that crosses a block boundary
 *     inside one call (the driver's chunked read regressed exactly there),
 *     the tail to end of file, a read at end of file, and the three seek
 *     origins;
 *   * enumeration: `/` lists `/system` and `/fs`, `/fs` lists the file the
 *     program created;
 *   * directory ops: mkdir, the DIR stat, rename, the renamed stat, the
 *     vanished old name, unlink and rmdir;
 *   * `/system`: open, stat and read a recovery-store file;
 *   * nested exec: `/system/fs_child.exe` gets the tail, writes through
 *     its own stdout, leaks every handle it opened and reports the exit
 *     status this program compares - the card slots the child leaked must
 *     be reusable immediately, which proves the kernel released them;
 *     the same child is also copied to the card and executed
 *     *from* `/fs`, so a card executable's path is read by a bank-6
 *     provider on the real machine;
 *   * a card that is absent or read-only skips the card outcomes, keeps
 *     `/system` working and still ends with a verdict.
 *
 * The replay reads the lines below; the final line is the verdict.  The
 * image then execs the monitor, so the transcript reaches the production
 * prompt.
 *
 * One tail option, `stage`: the same program then
 * runs as the hardware exerciser's files stage - every write confined to
 * the /fs/HWTEST scratch directory the runner creates and removes, the
 * nested exec tagging its child the same way, and a return to the caller
 * instead of the monitor handover.  The boot path above is unchanged, so
 * the fs image's transcript is exactly what it was.
 */

#include "kos.h"

#define FS_DIR   "/fs"
#define FS_FILE  "/fs/SELFTEST.TMP"
#define FS_RENAMED "/fs/RENAMED.TMP"
#define FS_SUBDIR "/fs/SELFTEST.D"
#define SYS_FILE "/system/drivers.man"
#define CHILD    "/system/fs_child.exe"

/* The stage tail and the scratch paths it selects. */
#define FS_STAGE_TAIL "stage"
#define HW_DIR    "/fs/HWTEST"
#define HW_FILE   "/fs/HWTEST/SELFTEST.TMP"
#define HW_RENAMED "/fs/HWTEST/RENAMED.TMP"
#define HW_SUBDIR "/fs/HWTEST/SELFTEST.D"

/* The child's copy on the card: the program this file
 * copies and then execs *from `/fs`*, which is the one way a card
 * executable is proven on the real machine. */
#define FS_CHILD_PROGRAM "/fs/CHILD.EXE"
#define HW_CHILD_PROGRAM "/fs/HWTEST/CHILD.EXE"

#define FS_BLOCK 512u
#define FS_SIZE  1200u

/* The child's exit status (tests/fs/fs_child.c). */
#define CHILD_STATUS 42

#define ENUM_PAGE 4u

static unsigned char pattern[FS_BLOCK];
static unsigned char buf[FS_BLOCK];
static struct kfs_stat st;
static struct kos_ns_entry entries[ENUM_PAGE];

static unsigned int checks;
static unsigned int failures;
/* EXE v2 has no initialized writable data: every static starts zeroed. */
static int card;
/* 1: the exerciser's files stage: scratch paths, no handover. */
static int stage;

static void check(const char *label, int ok)
{
	++checks;
	if (!ok) {
		++failures;
		printf("fs-selftest: FAIL %s\r\n", label);
		return;
	}
	printf("fs-selftest: %s OK\r\n", label);
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
		int count = list(path, &cookie, entries, ENUM_PAGE);
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
				 KOS_NS_ENTRY_CONTAINER) != 0u;

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
	int handle = open(path, flags);

	if (handle < 0) {
		return handle;
	}
	if (write((unsigned int)handle, pattern, FS_BLOCK) !=
	    (int)FS_BLOCK) {
		(void)close((unsigned int)handle);
		return -1;
	}
	if (write((unsigned int)handle, pattern, FS_BLOCK) !=
	    (int)FS_BLOCK) {
		(void)close((unsigned int)handle);
		return -1;
	}
	if (write((unsigned int)handle, pattern,
		      (unsigned int)(FS_SIZE - 2u * FS_BLOCK)) !=
	    (int)(FS_SIZE - 2u * FS_BLOCK)) {
		(void)close((unsigned int)handle);
		return -1;
	}
	if (sync((unsigned int)handle) != 0) {
		(void)close((unsigned int)handle);
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
	handle = card_write(file, (unsigned char)(KFS_O_WRITE | KFS_O_CREATE |
						  KFS_O_TRUNC));
	check("fs-open-create", handle >= 0);
	check("fs-write-blocks", handle >= 0);
	if (handle < 0) {
		return;
	}
	check("fs-close-write", close((unsigned int)handle) == 0);

	check("fs-stat-type",
	      stat(file, &st) == 0 && st.type == KFS_TYPE_FILE);
	check("fs-stat-size", st.size == FS_SIZE);

	/* read back */
	handle = open(file, KFS_O_READ);
	check("fs-open-read", handle >= 0);
	if (handle < 0) {
		return;
	}
	check("fs-read-head",
	      read((unsigned int)handle, buf, 128u) == 128 &&
	      slice_matches(0u, 128u));
	check("fs-seek-set",
	      seek((unsigned int)handle, 400, KFS_SEEK_SET) == 400);
	check("fs-read-cross-block",
	      read((unsigned int)handle, buf, 300u) == 300 &&
	      slice_matches(400u, 300u));
	check("fs-seek-end",
	      seek((unsigned int)handle, 0, KFS_SEEK_END) ==
	      (long)FS_SIZE);
	check("fs-read-eof",
	      read((unsigned int)handle, buf, 64u) == 0);
	check("fs-seek-cur",
	      seek((unsigned int)handle, -100, KFS_SEEK_CUR) ==
	      (long)(FS_SIZE - 100u));
	check("fs-read-tail",
	      read((unsigned int)handle, buf, 100u) == 100 &&
	      slice_matches(FS_SIZE - 100u, 100u));
	check("fs-close-read", close((unsigned int)handle) == 0);

	/* enumeration */
	check("enum-root-system", enum_has("/", "system", 1));
	check("enum-root-fs", enum_has("/", "fs", 1));
	check("enum-card-file", enum_has(dir, "SELFTEST.TMP", 0));

	/* directory ops */
	check("fs-mkdir", mkdir(subdir) == 0);
	check("fs-mkdir-stat",
	      stat(subdir, &st) == 0 && st.type == KFS_TYPE_DIR);
	check("enum-card-subdir", enum_has(dir, "SELFTEST.D", 1));
	check("fs-rename", rename(file, renamed) == 0);
	check("fs-rename-stat",
	      stat(renamed, &st) == 0 && st.size == FS_SIZE);
	check("fs-rename-old-gone", stat(file, &st) == -KOS_ENOENT);
	check("fs-unlink", unlink(renamed) == 0);
	check("fs-rmdir", rmdir(subdir) == 0);
	check("fs-unlink-gone", stat(renamed, &st) == -KOS_ENOENT);
}

/* --- the recovery store and the nested exec ------------------------------ */

static void system_case(void)
{
	int handle;
	unsigned char head[16];

	check("system-stat",
	      stat(SYS_FILE, &st) == 0 && st.type == KFS_TYPE_FILE &&
	      st.size != 0u);
	handle = open(SYS_FILE, KFS_O_READ);
	check("system-open", handle >= 0);
	if (handle < 0) {
		return;
	}
	check("system-read",
	      read((unsigned int)handle, head, sizeof(head)) ==
	      (int)sizeof(head));
	check("system-close", close((unsigned int)handle) == 0);
}

/* Copy the child program from the store to the card, so the exec below
 * runs an image that lives on the medium - the path a bank-6 provider has
 * to read.  Returns 1 when the copy completed. */
static int copy_child_to_card(const char *dest)
{
	unsigned int done = 0u;
	int source = open(CHILD, KFS_O_READ);
	int target;
	int ok = 1;

	if (source < 0) {
		return 0;
	}
	target = open(dest, (unsigned char)(KFS_O_WRITE | KFS_O_CREATE |
						KFS_O_TRUNC));
	if (target < 0) {
		(void)close((unsigned int)source);
		return 0;
	}
	for (;;) {
		int count = read((unsigned int)source, buf, FS_BLOCK);

		if (count <= 0) {
			ok = (count == 0);
			break;
		}
		if (write((unsigned int)target, buf,
			      (unsigned int)count) != count) {
			ok = 0;
			break;
		}
		done += (unsigned int)count;
	}
	if (ok) {
		ok = (sync((unsigned int)target) == 0);
	}
	(void)close((unsigned int)target);
	(void)close((unsigned int)source);
	if (!ok || done == 0u) {
		(void)unlink(dest);
		return 0;
	}
	return 1;
}

static void exec_case(void)
{
	int status = run(CHILD, stage ? FS_STAGE_TAIL : "tail-42");

	check("exec-status", status == CHILD_STATUS);

	/* The same child, run from the card: the exec bind hands the path to
	 * the bank-6 filesystem provider, so this row fails if exec stages it
	 * anywhere the provider's window cannot see. */
	if (card != 0) {
		const char *copy = stage ? HW_CHILD_PROGRAM
					 : FS_CHILD_PROGRAM;

		check("exec-card-copy", copy_child_to_card(copy));
		status = run(copy, stage ? FS_STAGE_TAIL : "tail-42");
		check("exec-card-status", status == CHILD_STATUS);
		if (stage != 0) {
			(void)unlink(copy);
		}
	}

	/* The child leaked a card file and a bound `/fs`: if the kernel had
	 * not released them, this open would report -KOS_EMFILE. */
	if (card != 0) {
		int handle = open(stage ? HW_FILE : FS_FILE,
				      (unsigned char)(KFS_O_WRITE |
						      KFS_O_CREATE));

		check("exec-child-handles-released", handle >= 0);
		if (handle >= 0) {
			(void)close((unsigned int)handle);
			(void)unlink(stage ? HW_FILE : FS_FILE);
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

/* The `truncate` tail: one scratch file driven through the truncate
 * operation - the shrink, the cache invalidation, the no-op rule, the
 * position clamp and the read-only refusals.  Its check lines are the
 * replay's count markers. */
#define TR_FILE "/fs/TRUNCATE.TMP"

static int truncate_case(void)
{
	int handle;
	int ro;
	long position;
	int result;

	make_pattern();
	handle = open(TR_FILE, (unsigned char)(KFS_O_WRITE | KFS_O_CREATE));
	check("trunc-open", handle >= 0);
	if (handle < 0) {
		return 1;
	}
	/* 3,000 bytes, the pattern repeated across the block boundary.
	 * The pattern buffer is one block, so the file fills in
	 * block-sized writes (one 440-byte tail). */
	result = 0;
	for (;;) {
		unsigned int n = (3000u - (unsigned int)result < FS_BLOCK)
					 ? (3000u - (unsigned int)result)
					 : FS_BLOCK;

		if (n == 0u) {
			break;
		}
		if (write((unsigned int)handle, pattern, n) != (int)n) {
			result = -1;
			break;
		}
		result = (int)((unsigned int)result + n);
	}
	check("trunc-write", result == 3000);
	/* A cached read at 1,500: the shrink must not leave these bytes
	 * readable afterwards. */
	position = seek((unsigned int)handle, 1500, KFS_SEEK_SET);
	check("trunc-cache-read",
	      position == 1500 &&
	      read((unsigned int)handle, buf, FS_BLOCK) == FS_BLOCK &&
	      slice_matches(1500u, FS_BLOCK));
	/* The shrink, then the old tail reads as end of file. */
	result = truncate((unsigned int)handle, 1000u);
	check("trunc-shrink", result == 0);
	(void)seek((unsigned int)handle, 1500, KFS_SEEK_SET);
	check("trunc-eof", read((unsigned int)handle, buf, FS_BLOCK) == 0);
	/* stat by path reads the directory entry, which takes the open
	 * file's size at a sync (or the close). */
	check("trunc-stat", sync((unsigned int)handle) == 0 &&
	      stat(TR_FILE, &st) == 0 && st.size == 1000u);
	/* The first 1,000 bytes are intact; 1,000 is end of file. */
	(void)seek((unsigned int)handle, 0, KFS_SEEK_SET);
	check("trunc-head",
	      read((unsigned int)handle, buf, FS_BLOCK) == (int)FS_BLOCK &&
	      slice_matches(0u, FS_BLOCK) &&
	      read((unsigned int)handle, buf, 488u) == 488 &&
	      slice_matches(FS_BLOCK, 488u));
	(void)seek((unsigned int)handle, 1000, KFS_SEEK_SET);
	check("trunc-end", read((unsigned int)handle, buf, FS_BLOCK) == 0);
	/* At or past the size: no-ops. */
	check("trunc-noop", truncate((unsigned int)handle, 1000u) == 0 &&
		truncate((unsigned int)handle, 5000u) == 0);
	/* A position past the new length clamps to it. */
	(void)seek((unsigned int)handle, 1500, KFS_SEEK_SET);
	result = truncate((unsigned int)handle, 500u);
	position = seek((unsigned int)handle, 0, KFS_SEEK_CUR);
	check("trunc-position", result == 0 && position == 500);
	/* A read-only handle refuses; the file is unchanged. */
	ro = open(TR_FILE, KFS_O_READ);
	check("trunc-ro-handle",
	      ro >= 0 && truncate((unsigned int)ro, 10u) < 0);
	if (ro >= 0) {
		(void)close((unsigned int)ro);
	}
	check("trunc-ro-unchanged", sync((unsigned int)handle) == 0 &&
	      stat(TR_FILE, &st) == 0 && st.size == 500u);
	/* The read-only provider refuses the same way. */
	ro = open(SYS_FILE, KFS_O_READ);
	check("trunc-ro-system",
	      ro >= 0 && truncate((unsigned int)ro, 10u) < 0);
	if (ro >= 0) {
		(void)close((unsigned int)ro);
	}
	/* The `call` gate: an op the FILE gate does not forward (0xEE)
	 * is refused with -KOS_EINVAL.  The refusal must surface as the
	 * call's result - the reply status field stays 0 on a refusal,
	 * so returning the status instead would report success here. */
	{
		struct kos_ipc_request req;

		req.frame.op = 0xEEu;
		req.frame.flags = 0u;
		req.frame.desc = KOS_IPC_DESC_NONE;
		req.frame.arg0 = 0u;
		req.frame.arg1 = 0u;
		req.buffer = (void *)0;
		req.length = 0u;
		req.direction = KOS_IPC_DESC_NONE;
		check("trunc-call-refusal",
		      call((unsigned char)handle, &req) == -KOS_EINVAL);
	}
	(void)close((unsigned int)handle);
	(void)unlink(TR_FILE);
	printf("fs-selftest: truncate %u checks, %u failures\r\n", checks,
	       failures);
	console_puts(failures == 0u ? "fs-selftest: truncate PASS\r\n"
				    : "fs-selftest: truncate FAIL\r\n");
	return (failures == 0u) ? 0 : 1;
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
	struct kos_startup *boot = startup();
	const char *tail = (const char *)0;
	int probe;

	/* The stage tail selects the scratch paths and suppresses the
	 * monitor handover; the truncate tail runs only the truncate
	 * checks.  Anything else is refused before a check runs. */
	if (argc == 1 && argv != (char **)0 && same(argv[0], FS_STAGE_TAIL)) {
		stage = 1;
		tail = FS_STAGE_TAIL;
	} else if (argc == 1 && argv != (char **)0 &&
		   same(argv[0], "truncate")) {
		return truncate_case();
	} else if (argc != 0) {
		console_puts("fs-selftest: FAIL options\r\n");
		return 2;
	}

	console_puts("fs-selftest: begin\r\n");
	make_pattern();
	card = 1;

	/* Boot contract: version 1 with the empty boot tail.  Run as
	 * the exerciser's stage, the same line proves the option tail
	 * arrived byte-exact instead. */
	check("startup-version",
	      boot != (struct kos_startup *)0 && boot->version == 1u);
	check("startup-tail",
	      boot != (struct kos_startup *)0 &&
	      (tail == (const char *)0
		       ? boot->length == 0u
		       : (boot->length == (unsigned char)length_of(tail) &&
			  same(boot->tail, tail))));
	/* The single `=` is the whole test: one byte through the standard
	 * handle must come back as 1.  The byte itself is therefore the
	 * glyph that appears on the display immediately before this row:
	 * the stray `=` is this check, not a console fault. */
	check("console-write", console_write("=", 1u) == 1);

	/* A card that is absent or write-protected must leave `/system`
	 * usable and the verdict honest: probe with a read-only open, which
	 * reports -KOS_ENODEV when no card answers and -KOS_EROFS for a
	 * write-protected one. */
	probe = open(stage ? HW_FILE : FS_FILE, KFS_O_READ);
	if (probe == -KOS_ENODEV) {
		card = 0;
		console_puts("fs-selftest: card absent: /fs not served\r\n");
	} else if (probe == -KOS_EROFS || probe == -KOS_EINVAL) {
		card = 0;
		console_puts("fs-selftest: card not writable\r\n");
	} else if (probe >= 0) {
		(void)close((unsigned int)probe);
	} else if (probe != -KOS_ENOENT) {
		card = 0;
		console_puts("fs-selftest: /fs not served\r\n");
	}

	if (card != 0) {
		card_case();
	}
	system_case();
	exec_case();

	printf("fs-selftest: %u checks, %u failures\r\n", checks,
		   failures);
	if (failures == 0u) {
		console_puts("fs-selftest: PASS\r\n");
	} else {
		console_puts("fs-selftest: FAIL\r\n");
	}

	/* Production-like boot: hand the console to the monitor.  A stage
	 * run returns to the runner that started it. */
	if (stage == 0) {
		(void)run("/system/monitor.exe", "");
	}
	return (failures == 0u) ? 0 : 1;
}
