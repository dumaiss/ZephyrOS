/* KonaOS feature exerciser.
 *
 * `/system/hwtest.exe` is the production store's runner: the owner starts
 * it from the monitor (`X /system/hwtest.exe`) on the real machine and
 * reads one line per stage plus the verdict.  It drives the same outcome
 * programs the acceptance images boot, so the mechanisms stay one
 * implementation:
 *
 *   xmem+far  `/system/xmem_selftest.exe` with the `stage` tail (the
 *             extended-memory rows, stated against the banks this context
 *             opens) and then `/system/xmem_far.exe` directly (two far
 *             segments through the aperture, unmodified): the far program
 *             runs one exec level shallower than it would below the
 *             selftest, because the chain monitor -> runner -> stage
 *             already sits at the kernel's four-level exec limit.  Both
 *             runs are bracketed by this runner's own whole-bank counts,
 *             so each child's exit must return every bank it held;
 *   files     `/system/fs_selftest.exe` with the `stage` tail: the same
 *             checks with every write inside `/fs/HWTEST`, which this
 *             runner creates and removes - also after a failure - so the
 *             card keeps no trace.  With no card (or a read-only one) the
 *             absent-card branch is the expected result and the line
 *             says so;
 *   namespace the checks of `tests/ns/ns.c` that apply to the real
 *   +handles  publications: the five `/dev` objects with their types,
 *             enumeration of `/`, `/dev`, `/system` and the card root,
 *             CWD traversal including `/`, a bad path and a bad handle,
 *             and handle-table exhaustion with recovery.
 *
 * Nested exec is covered by the two program stages (a tail, an exit
 * status, and the child's handles released before the next run).
 *
 * Output contract: every line is `hwtest: ...`, the stage lines carry
 * PASS, `FAIL <label>`, `card absent` or `card not writable`, and the
 * last line is `hwtest: <passed>/3 PASS|FAIL` with ` (card absent)` or
 * ` (card not writable)` appended when that is the card state.  A failed
 * row prints the observed code next to the label - `FAIL bind-container
 * got 02` for a wrong status, `FAIL cwd-root type 02 want 04` for a
 * wrong object type - so the owner reads which side answered what
 * without a second run.  Each
 * line goes to the console (the V9958 in production) and, when the store
 * publishes a SERIAL object at `/dev/serial/0`, to that lane too, so a
 * terminal or `tools/serial_smoke.py --mode hwtest` reads the same
 * transcript the owner sees.
 *
 * The runner holds no handle across a stage's `SYS_EXEC` (the lane is
 * released and re-taken around it), and it returns 0 on PASS and 1 on
 * FAIL - the status the monitor prints.
 */

#include "kos.h"
#include "klibc/fmt.h"
#include "klibc/string.h"
#include "kos/protocol.h"

#define LINE_MAX 96

#define LANE_NAME    "/dev/serial/0"
#define XMEM_PROGRAM "/system/xmem_selftest.exe"
#define FAR_PROGRAM  "/system/xmem_far.exe"
#define FS_PROGRAM   "/system/fs_selftest.exe"
#define XMEM_STAGE_TAIL "stage"
#define FS_STAGE_TAIL   "stage"

/* tests/xmem/xmem_far.c exits with this status when every row passed. */
#define FAR_STATUS 43u

#define FS_DIR    "/fs"
#define HW_DIR    "/fs/HWTEST"
#define HW_FILE   "/fs/HWTEST/SELFTEST.TMP"
#define HW_RENAMED "/fs/HWTEST/RENAMED.TMP"
#define HW_SUBDIR "/fs/HWTEST/SELFTEST.D"
#define HW_CHILD  "/fs/HWTEST/CHILD.TMP"
#define HW_PROGRAM "/fs/HWTEST/CHILD.EXE"

/* kernel/vfs/vfs.h VFS_HANDLE_MAX: the process handle pool. */
#define FD_MAX 12u

/* A handle value no process can own. */
#define BAD_HANDLE 90u

/* Four entries per page: 112 bytes of caller buffer. */
#define ENUM_PAGE 4u

#define STAGE_COUNT 3u

/* Card states the stage lines and the summary carry. */
#define CARD_PRESENT  0
#define CARD_ABSENT   1
#define CARD_READONLY 2

static char hw_line[LINE_MAX];
static unsigned char hw_lane;
static struct kos_ipc_request req;
static struct kos_ns_entry entries[ENUM_PAGE];
static struct kfs_stat st;

/* The runner's own name: every line, so the transcript is unambiguous. */
static const char hw_name[] = "hwtest";

/* Stage 3 accumulates: a failing check reports only through the stage
 * line, whose detail is the first failing row - with the number the
 * system actually answered when the row compares codes, so a device that
 * reports a different error is diagnosable from the transcript alone
 *. */
#define NS_DETAIL_MAX 72
static unsigned int ns_failures;
static char ns_fail_detail[NS_DETAIL_MAX];

/* --- output: the console and the lane mirror --------------------------- */

static void hw_lane_write(const char *text, unsigned int length)
{
	req.frame.op = KSIO_OP_WRITE;
	req.frame.flags = 0u;
	req.frame.arg0 = 0u;
	req.frame.arg1 = length;
	req.buffer = (void *)text;
	req.length = length;
	req.direction = KOS_IPC_DESC_IN;
	if (call(hw_lane, &req) < 0) {
		hw_lane = 0u; /* the lane failed: the console remains */
	}
}

/* One line to both sinks: the console first, so a broken lane cannot cost
 * the owner the visible transcript. */
static void hw_write(const char *text, unsigned int length)
{
	(void)console_write(text, length);
	if (hw_lane != 0u) {
		hw_lane_write(text, length);
	}
}

static void hw_send(void)
{
	hw_write(hw_line, (unsigned int)strlen(hw_line));
}

static void hw_lane_open(void)
{
	unsigned char type = 0u;
	int handle = bind(LANE_NAME, 0u, &type);

	if (handle >= (int)KOS_HANDLE_FIRST_BOUND && type == KOS_TYPE_SERIAL) {
		hw_lane = (unsigned char)handle;
	}
}

static void hw_lane_close(void)
{
	if (hw_lane != 0u) {
		(void)unbind(hw_lane);
		hw_lane = 0u;
	}
}

/* --- stage lines -------------------------------------------------------- */

static void hw_stage_verdict(const char *name, const char *verdict,
			     const char *detail)
{
	if (detail != (const char *)0) {
		(void)snprintf(hw_line, sizeof(hw_line),
				   "hwtest: %s %s %s\r\n", name, verdict,
				   detail);
	} else {
		(void)snprintf(hw_line, sizeof(hw_line),
				   "hwtest: %s %s\r\n", name, verdict);
	}
	hw_send();
}

/* A stage program that did not pass: `status` is the child's u16 exit
 * status, or the negated native code when the exec itself failed. */
static void hw_stage_status(const char *name, int status)
{
	if (status < 0) {
		(void)snprintf(hw_line, sizeof(hw_line),
				   "hwtest: %s FAIL error 0x%02x\r\n", name,
				   (unsigned int)(-status) & 0xFFu);
	} else {
		(void)snprintf(hw_line, sizeof(hw_line),
				   "hwtest: %s FAIL status 0x%04x\r\n",
				   name, (unsigned int)status & 0xFFFFu);
	}
	hw_send();
}

/* Run one stage program synchronously with no handle held across it. */
static int hw_run(const char *path, const char *tail)
{
	int status;

	hw_lane_close();
	status = run(path, tail);
	hw_lane_open();
	return status;
}

/* --- stage 1: extended memory and far code ----------------------------- */

/* How many whole application banks this process can open right now: a
 * 32 KiB object needs one bank's plain slots, so counting them counts the
 * banks the monitor's and the runner's live processes leave free (4.8:
 * a bank with a live process is closed to auxiliary placement). */
static unsigned int hw_pool_banks(void)
{
	unsigned int held[8];
	unsigned int hwtest_open = 0u;
	unsigned int i;
	int handle;

	while (hwtest_open < 8u) {
		handle = xalloc(32768u, 0u);
		if (handle < 1) {
			break;
		}
		held[hwtest_open++] = (unsigned int)handle;
	}
	for (i = 0u; i < hwtest_open; ++i) {
		(void)xfree(held[i]);
	}
	return hwtest_open;
}

/* The extended-memory rows run in `/system/xmem_selftest.exe`; the far
 * rows run in `/system/xmem_far.exe` directly rather than nested below it:
 * the chain is already monitor -> runner -> stage and the kernel
 * allows four, so the far program's own nested exec would be the fifth.
 * The two pool counts around each run are this runner's own evidence that
 * a child's exit returns everything it held - banks, objects and
 * segments. */
static int hw_stage_xmem(void)
{
	const unsigned int before = hw_pool_banks();
	int status = hw_run(XMEM_PROGRAM, XMEM_STAGE_TAIL);

	if (status != 0) {
		hw_stage_status("xmem+far", status);
		return 0;
	}
	if (hw_pool_banks() != before) {
		hw_stage_verdict("xmem+far", "FAIL", "stage-released");
		return 0;
	}
	status = hw_run(FAR_PROGRAM, "");
	if (status != (int)FAR_STATUS) {
		hw_stage_status("xmem+far", status);
		return 0;
	}
	if (hw_pool_banks() != before) {
		hw_stage_verdict("xmem+far", "FAIL", "far-released");
		return 0;
	}
	hw_stage_verdict("xmem+far", "PASS", (const char *)0);
	return 1;
}

/* --- stage 2: the card, inside the scratch directory ------------------- */

/* Remove the stage's whole card footprint: the files the programs may
 * have left, the directory they made, and finally the scratch directory
 * itself, so the card ends exactly as the stage found it. */
static void hw_scratch_remove(void)
{
	(void)unlink(HW_FILE);
	(void)unlink(HW_RENAMED);
	(void)unlink(HW_CHILD);
	(void)unlink(HW_PROGRAM);
	(void)rmdir(HW_SUBDIR);
	(void)rmdir(HW_DIR);
}

/* Create the scratch directory: 0 when the card answered, otherwise the
 * state the verdict carries.  A directory left behind by an earlier
 * interrupted run is cleared once and retried. */
static int hw_scratch_make(void)
{
	int result = mkdir(HW_DIR);

	if (result != 0) {
		hw_scratch_remove();
		result = mkdir(HW_DIR);
	}
	if (result == 0) {
		return CARD_PRESENT;
	}
	if (result == -KOS_ENODEV) {
		return CARD_ABSENT;
	}
	return CARD_READONLY;
}

/* Returns 1 when the stage passed and reports the card state through
 * `*card`. */
static int hw_stage_files(int *card)
{
	int status;

	*card = hw_scratch_make();
	if (*card == CARD_PRESENT) {
		status = hw_run(FS_PROGRAM, FS_STAGE_TAIL);
		if (status != 0) {
			hw_stage_status("files", status);
			hw_scratch_remove(); /* also after a failure */
			return 0;
		}
		hw_scratch_remove();
		if (stat(HW_DIR, &st) != -KOS_ENOENT) {
			hw_stage_verdict("files", "FAIL", "scratch-removed");
			return 0;
		}
		hw_stage_verdict("files", "PASS", (const char *)0);
		return 1;
	}

	/* No card (or a read-only one): the absent-card branch is the
	 * expected result, and the line says which state applied. */
	status = hw_run(FS_PROGRAM, FS_STAGE_TAIL);
	if (status != 0) {
		hw_stage_status("files", status);
		return 0;
	}
	hw_stage_verdict("files",
			 (*card == CARD_ABSENT) ? "card absent"
						: "card not writable",
			 (const char *)0);
	return 1;
}

/* --- stage 3: namespace and handles ------------------------------------ */

/* Record one failure; only the first reaches the verdict line. */
static void hw_fail(const char *label, const char *detail)
{
	if (ns_failures == 0u) {
		if (detail != (const char *)0) {
			(void)snprintf(ns_fail_detail, sizeof(ns_fail_detail),
					   "%s %s", label, detail);
		} else {
			(void)snprintf(ns_fail_detail, sizeof(ns_fail_detail),
					   "%s", label);
		}
	}
	++ns_failures;
}

/* A boolean row. */
static void hw_check(const char *label, int ok)
{
	if (ok == 0) {
		hw_fail(label, (const char *)0);
	}
}

/* The two hex digits of a call result, as the monitor's `ERR:` row writes
 * them: the negated native code, or 00 for an accepted call. */
static unsigned int hw_code(int status)
{
	return (status < 0) ? ((unsigned int)(-status) & 0xFFu) : 0u;
}

/* A row that compares codes: the detail carries the answer and the
 * expected code. */
static void hw_check_status(const char *label, int got, int want)
{
	char detail[24];

	if (got == want) {
		return;
	}
	(void)snprintf(detail, sizeof(detail), "got %02x want %02x",
			   hw_code(got), hw_code(want));
	hw_fail(label, detail);
}

/* The code an accepted call must not answer with. */
static void hw_check_accepted(const char *label, int got)
{
	char detail[24];

	if (got >= 0) {
		return;
	}
	(void)snprintf(detail, sizeof(detail), "got %02x", hw_code(got));
	hw_fail(label, detail);
}

/* An enumeration row: 1 when the name was listed, else the code or the
 * missing name. */
static void hw_check_enum(const char *label, int result)
{
	if (result == 1) {
		return;
	}
	if (result < 0) {
		hw_check_accepted(label, result);
		return;
	}
	hw_fail(label, "not listed");
}

/* Page `path` and report whether `name` appears as a container or as a
 * leaf (the fs acceptance program's helper, reused): 1 found, 0 not
 * listed, or the first negative code the provider answered. */
static int hw_enum_has(const char *path, const char *name, int container)
{
	unsigned int cookie = 0u;
	unsigned int pages = 0u;

	for (;;) {
		int count = list(path, &cookie, entries, ENUM_PAGE);
		int index;

		if (count < 0) {
			return count;
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

/* Page `path` once: 0 when the provider answered, else its code. */
static int hw_enum_status(const char *path)
{
	unsigned int cookie = 0u;
	int result = list(path, &cookie, entries, ENUM_PAGE);

	return (result >= 0) ? 0 : result;
}

/* Bind `path`, check the type it carries and release it again. */
static void hw_check_bind(const char *label, const char *path,
			  unsigned char type)
{
	unsigned char bound_type = 0u;
	int handle = bind(path, 0u, &bound_type);

	if (handle < (int)KOS_HANDLE_FIRST_BOUND) {
		hw_check_accepted(label, handle);
		return;
	}
	if (bound_type != type) {
		char detail[24];

		(void)snprintf(detail, sizeof(detail),
				   "type %02x want %02x",
				   (unsigned int)bound_type,
				   (unsigned int)type);
		hw_fail(label, detail);
	}
	(void)unbind((unsigned char)handle);
}

/* Fill this process's table until the pool refuses, then release it. */
static unsigned int hw_fill_table(unsigned char *handles, int *last)
{
	unsigned char type = 0u;
	unsigned int count = 0u;

	*last = 0;
	while (count < FD_MAX) {
		int result = bind("/dev/console", 0u, &type);

		if (result < 0) {
			*last = result;
			break;
		}
		handles[count] = (unsigned char)result;
		++count;
	}
	return count;
}

static void hw_release_table(unsigned char *handles, unsigned int count)
{
	unsigned int index;

	for (index = 0u; index < count; ++index) {
		(void)unbind(handles[index]);
	}
}

/* The descriptor pool is one fixed-core table for every owner
 * (kernel/vfs/vfs.h), so the monitor's bound handles are part of it too
 * and the number this process can hold is the documented 12 minus what
 * the live processes already hold.  The stage therefore pins the
 * behaviour - the fill ends on EMFILE, every slot comes back, the same
 * capacity is there afterwards - instead of a count the monitor decides. */
static void hw_check_table(void)
{
	unsigned char handles[FD_MAX];
	unsigned char again[FD_MAX];
	unsigned char type = 0u;
	unsigned int count;
	unsigned int second;
	int result;

	count = hw_fill_table(handles, &result);
	hw_check("table-fills", count >= 3u && count < FD_MAX);
	hw_check_status("table-emfile", result, -KOS_EMFILE);
	hw_release_table(handles, count);

	second = hw_fill_table(again, &result);
	hw_check_status("table-restored", result, -KOS_EMFILE);
	hw_check("table-restored-count", second == count);
	hw_release_table(again, second);

	result = bind("/dev/console", 0u, &type);
	hw_check_accepted("table-reuse", result);
	if (result >= (int)KOS_HANDLE_FIRST_BOUND) {
		hw_check_status("table-reuse-unbind",
				unbind((unsigned char)result), 0);
	}
}

static void hw_stage_namespace(int card)
{
	char cwd[16];
	unsigned char type = 0u;
	int result;

	ns_failures = 0u;
	ns_fail_detail[0] = '\0';
	/* A clean table for the exhaustion row: the runner's own lane
	 * handle would otherwise occupy one of the twelve slots. */
	hw_lane_close();

	/* The real production publications and the type each carries. */
	hw_check_bind("bind-dev-console", "/dev/console", KOS_TYPE_CONSOLE);
	hw_check_bind("bind-dev-video", "/dev/video", KOS_TYPE_VIDEO);
	hw_check_bind("bind-dev-input", "/dev/input/keyboard",
		      KOS_TYPE_INPUT);
	hw_check_bind("bind-dev-serial", "/dev/serial/0", KOS_TYPE_SERIAL);
	hw_check_bind("bind-dev-ioc", "/dev/ioc", KOS_TYPE_IOC);

	/* Refusals: a grouping entry is not bindable, an absent name and an
	 * absent card are distinct, and an empty path is malformed. */
	hw_check_status("bind-container", bind("/dev", 0u, &type),
			-KOS_EISDIR);
	hw_check_status("bind-container-input",
			bind("/dev/input", 0u, &type), -KOS_EISDIR);
	hw_check_status("bind-unknown-device",
			bind("/dev/nosuch", 0u, &type),
			-KOS_ENOENT);
	hw_check_status("bind-unknown-file",
			bind("/system/nosuch.exe", 0u, &type),
			-KOS_ENOENT);
	hw_check_status("bind-empty-einval", bind("", 0u, &type),
			-KOS_EINVAL);
	if (card == CARD_PRESENT) {
		hw_check_status("bind-card-absent-name",
				bind("/fs/nosuch", 0u, &type),
				-KOS_ENOENT);
	} else {
		hw_check_status("bind-card-enodev",
				bind("/fs/nosuch", 0u, &type),
				-KOS_ENODEV);
	}

	/* Enumeration: the root and the device tree hold the production
	 * publications, the store holds the exerciser and its programs, and
	 * a card answers below /fs. */
	hw_check_enum("enum-root-system", hw_enum_has("/", "system", 1));
	hw_check_enum("enum-root-fs", hw_enum_has("/", "fs", 1));
	hw_check_enum("enum-dev-console", hw_enum_has("/dev", "console", 0));
	hw_check_enum("enum-dev-video", hw_enum_has("/dev", "video", 0));
	hw_check_enum("enum-dev-ioc", hw_enum_has("/dev", "ioc", 0));
	hw_check_enum("enum-dev-input", hw_enum_has("/dev", "input", 1));
	hw_check_enum("enum-dev-serial", hw_enum_has("/dev", "serial", 1));
	hw_check_enum("enum-system-monitor",
		      hw_enum_has("/system", "monitor.exe", 0));
	hw_check_enum("enum-system-hwtest",
		      hw_enum_has("/system", "hwtest.exe", 0));
	hw_check_enum("enum-system-xmem",
		      hw_enum_has("/system", "xmem_selftest.exe", 0));
	hw_check_enum("enum-system-fs",
		      hw_enum_has("/system", "fs_selftest.exe", 0));
	if (card == CARD_PRESENT) {
		hw_check_status("enum-card", hw_enum_status(FS_DIR), 0);
	}

	/* CWD: the root is traversable, the store and the card root are
	 * locations, an object is not, and a refusal leaves the CWD alone. */
	hw_check_status("chdir-root", chdir("/"), 0);
	result = getcwd(cwd, sizeof(cwd));
	hw_check_status("cwd-root", result, 1);
	hw_check("cwd-root-path", strcmp(cwd, "/") == 0);
	hw_check_status("chdir-system", chdir("/system"), 0);
	result = getcwd(cwd, sizeof(cwd));
	hw_check_status("cwd-system", result, 7);
	hw_check("cwd-system-path", strcmp(cwd, "/system") == 0);
	hw_check_bind("cwd-relative-bind", "drivers.man", KOS_TYPE_FILE);
	if (card == CARD_PRESENT) {
		hw_check_status("chdir-card", chdir(FS_DIR), 0);
		result = getcwd(cwd, sizeof(cwd));
		hw_check_status("cwd-card", result, 3);
		hw_check("cwd-card-path", strcmp(cwd, FS_DIR) == 0);
	} else {
		hw_check_status("chdir-card-enodev", chdir(FS_DIR),
				-KOS_ENODEV);
	}
	/* The two refusals must leave the CWD where it was: `/system` without
	 * a card, the card root with one. */
	hw_check_status("chdir-object-refused", chdir("/dev/console"),
			-KOS_ENOTDIR);
	hw_check_status("chdir-absent", chdir("/dev/nosuch"),
			-KOS_ENOENT);
	result = getcwd(cwd, sizeof(cwd));
	hw_check_status("cwd-survives-refusal", result,
			(card == CARD_PRESENT) ? 3 : 7);
	hw_check("cwd-survives-refusal-path",
		 strcmp(cwd, (card == CARD_PRESENT) ? FS_DIR : "/system") == 0);

	/* Bad handles: a value no process owns is refused by both the call
	 * and the release path. */
	req.frame.op = KCONSOLE_OP_POLL;
	req.frame.flags = 0u;
	req.frame.arg0 = 0u;
	req.frame.arg1 = 0u;
	req.buffer = (void *)0;
	req.length = 0u;
	req.direction = KOS_IPC_DESC_IN;
	hw_check_status("call-bad-handle", call(BAD_HANDLE, &req),
			-KOS_EINVAL);
	hw_check_status("unbind-bad-handle", unbind(BAD_HANDLE),
			-KOS_EINVAL);
	hw_check_status("unbind-zero", unbind(0u), -KOS_EINVAL);

	/* The table fills to its documented size, refuses with EMFILE, and
	 * every slot comes back. */
	hw_check_table();

	/* Leave the CWD at the root and take the lane back for the verdict. */
	(void)chdir("/");
	hw_lane_open();
}

/* --- the runner --------------------------------------------------------- */

int main(int argc, char **argv)
{
	const char *note = "";
	unsigned int passed = 0u;
	int card = CARD_PRESENT;

	(void)argc;
	(void)argv;

	hw_lane_open();
	(void)snprintf(hw_line, sizeof(hw_line), "%s: begin\r\n",
			   hw_name);
	hw_send();

	if (hw_stage_xmem() != 0) {
		++passed;
	}
	if (hw_stage_files(&card) != 0) {
		++passed;
	}
	hw_stage_namespace(card);
	if (ns_failures == 0u) {
		++passed;
		hw_stage_verdict("namespace+handles", "PASS", (const char *)0);
	} else {
		hw_stage_verdict("namespace+handles", "FAIL",
				 ns_fail_detail);
	}

	if (card == CARD_ABSENT) {
		note = " (card absent)";
	} else if (card == CARD_READONLY) {
		note = " (card not writable)";
	}
	(void)snprintf(hw_line, sizeof(hw_line), "%s: %u/%u %s",
			   hw_name, passed, STAGE_COUNT,
			   (passed == STAGE_COUNT) ? "PASS" : "FAIL");
	(void)strlcat(hw_line, note, sizeof(hw_line));
	(void)strlcat(hw_line, "\r\n", sizeof(hw_line));
	hw_send();

	return (passed == STAGE_COUNT) ? 0 : 1;
}
