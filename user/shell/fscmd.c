/* KonaOS native shell: the file built-ins.
 *
 * `ls cat stat cp mv rm mkdir rmdir` run in the shell's own process on
 * the SDK's file and namespace words only; the providers do the work.
 * Paths are translated before use and shown in display form in every
 * message, exactly like the rest of the shell, and a failure prints
 * `<command>: <path>: <message>` and is the status.
 *
 * `mv` is the one policy the system does not offer: a rename that
 * crosses providers becomes a copy and an unlink, and when the unlink
 * fails (a `/system` source is read-only) the copy stays and its error
 * is reported - recovery without storage.
 */

#include "shell.h"

/* The one shared transfer buffer (requirement 1): `cat` and `cp` never
 * hold a file whole. */
static uint8_t io_buf[512];

/* The current command's paths, translated.  The shell is single-tasked,
 * so these are static and the whole 512-byte stack stays for the call
 * chain; the display buffer is one of them too. */
static char fscmd_src[KOS_PATH_MAX];
static char fscmd_dst[KOS_PATH_MAX];
static char fscmd_target[KOS_PATH_MAX];
static char fscmd_child[KOS_PATH_MAX];
static char fscmd_cwd[KOS_PATH_MAX];
static char fscmd_shown[KOS_PATH_MAX];

/* The `rm`/`mkdir`/`rmdir` leaf calls, one operation number each. */
#define FSCMD_UNLINK 0
#define FSCMD_MKDIR  1
#define FSCMD_RMDIR  2

static int fscmd_usage(const char *synopsis)
{
	printf("usage: %s\r\n", synopsis);
	return KOS_EINVAL;
}

/* The display form of a translated path, or the path itself when it is
 * too long to be shown. */
static const char *fscmd_display(const char *path)
{
	if (shell_path_display(path, fscmd_shown, sizeof fscmd_shown) < 0) {
		return path;
	}
	return fscmd_shown;
}

/* `<command>: <path>: <message>` over a translated path, and the
 * positive status. */
static int fscmd_error(const char *command, const char *path, int code)
{
	printf("%s: %s: %s\r\n", command, fscmd_display(path),
	       shell_strerror(code));
	return code;
}

/* The same over a word that never became a path (only a length can
 * fail, and the user's own word is the one worth showing). */
static int fscmd_error_word(const char *command, const char *word, int code)
{
	printf("%s: %s: %s\r\n", command, word, shell_strerror(code));
	return code;
}

/* One argument to a translated path in `out`; the error is already
 * reported and positive. */
static int fscmd_translate(const char *command, const char *word, char *out)
{
	if (shell_path_translate(word, out, KOS_PATH_MAX) < 0) {
		return fscmd_error_word(command, word, KOS_EINVAL);
	}
	return 0;
}

/* <dir>/<name> into `out`, or -KOS_EINVAL when it cannot fit. */
static int fscmd_join(const char *dir, const char *name, char *out,
		      unsigned int cap)
{
	unsigned int needed = strlen(dir) + 1u + strlen(name);

	if (needed + 1u > cap) {
		return -KOS_EINVAL;
	}
	(void)strlcpy(out, dir, cap);
	(void)strlcat(out, "/", cap);
	(void)strlcat(out, name, cap);
	return 0;
}

/* The last component of `path`, or the path itself when it has none. */
static const char *fscmd_basename(const char *path)
{
	const char *slash = strrchr(path, '/');

	return (slash != (const char *)0 && slash[1] != '\0') ? slash + 1
							     : path;
}

/* --- ls ----------------------------------------------------------------- */

/* The name and decimal size line a file gets, listing and alone. */
static void fscmd_ls_line(const char *name, const struct kfs_stat *st)
{
	printf("%s  %lu\r\n", name, st->size);
}

/* One entry of a listing: a container gets a trailing `/`, a file gets
 * its size from `stat` - skipped when that does not answer, which is
 * every `/dev` entry. */
static void fscmd_ls_entry(const char *dir, const struct kos_ns_entry *entry)
{
	struct kfs_stat st;

	if ((entry->flags & KOS_NS_ENTRY_CONTAINER) != 0u) {
		printf("%s/\r\n", entry->name);
		return;
	}
	if (fscmd_join(dir, entry->name, fscmd_child,
		       KOS_PATH_MAX) == 0 &&
	    stat(fscmd_child, &st) == 0) {
		fscmd_ls_line(entry->name, &st);
		return;
	}
	printf("%s\r\n", entry->name);
}

/* Page the namespace until it reports the end, a batch per call (the
 * card's provider answers the stats of the batch it just listed from
 * memory). */
#define FSCMD_LS_BATCH 8u

static struct kos_ns_entry fscmd_ls_batch[FSCMD_LS_BATCH];

static int fscmd_ls_entries(const char *dir)
{
	unsigned int cookie = 0;
	int status;
	int i;

	for (;;) {
		status = list(dir, &cookie, fscmd_ls_batch, FSCMD_LS_BATCH);
		if (status <= 0) {
			return status;
		}
		for (i = 0; i < status; ++i) {
			fscmd_ls_entry(dir, &fscmd_ls_batch[i]);
		}
	}
}

/* One `ls` argument: a file gets its single line, everything else is a
 * container to enumerate.  `stat` does not answer for every container
 * (`/` and `/dev` are the namespace's own), so a failed `stat` is not
 * an error until the listing fails too. */
static int fscmd_ls_one(const char *path)
{
	struct kfs_stat st;

	if (stat(path, &st) == 0 && st.type == KFS_TYPE_FILE) {
		fscmd_ls_line(fscmd_basename(fscmd_display(path)), &st);
		return 0;
	}
	return fscmd_ls_entries(path);
}

static int fscmd_ls(int argc, char **argv)
{
	int status = 0;
	int index;

	if (argc == 1) {
		status = fscmd_translate("ls", ".", fscmd_src);
		if (status != 0) {
			return status;
		}
		return fscmd_ls_one(fscmd_src);
	}
	for (index = 1; index < argc; ++index) {
		int result = fscmd_translate("ls", argv[index], fscmd_src);

		if (result != 0) {
			status = result;
			continue;
		}
		if (argc > 2) {
			/* Several arguments: each listing says which path
			 * it is. */
			printf("%s:\r\n", fscmd_display(fscmd_src));
		}
		result = fscmd_ls_one(fscmd_src);
		if (result < 0) {
			status = fscmd_error("ls", fscmd_src, -result);
		}
	}
	return status;
}

/* --- cat ---------------------------------------------------------------- */

/* One file to the console, raw, in chunks of at most the shared buffer
 * (requirement 3). */
static int fscmd_cat_one(const char *path)
{
	int handle = open(path, KFS_O_READ);
	int status;

	if (handle < 0) {
		return fscmd_error("cat", path, -handle);
	}
	for (;;) {
		status = read((unsigned int)handle, io_buf, sizeof io_buf);
		if (status <= 0) {
			break;                     /* end of file, or failed */
		}
		status = console_write((const char *)io_buf,
				       (unsigned int)status);
		if (status < 0) {
			break;
		}
	}
	(void)close((unsigned int)handle);
	if (status < 0) {
		return fscmd_error("cat", path, -status);
	}
	return 0;
}

static int fscmd_cat(int argc, char **argv)
{
	int status = 0;
	int index;

	if (argc < 2) {
		return fscmd_usage("cat file ...");
	}
	for (index = 1; index < argc; ++index) {
		int result = fscmd_translate("cat", argv[index], fscmd_src);

		if (result != 0) {
			status = result;
			continue;
		}
		result = fscmd_cat_one(fscmd_src);
		if (result != 0) {
			status = result;
		}
	}
	return status;
}

/* --- stat --------------------------------------------------------------- */

/* One `struct kfs_stat` as four lines under its own path
 * (requirement 4). */
static int fscmd_stat_one(const char *path)
{
	struct kfs_stat st;
	int status = stat(path, &st);

	if (status < 0) {
		return fscmd_error("stat", path, -status);
	}
	printf("%s:\r\n", fscmd_display(path));
	if (st.type == KFS_TYPE_FILE) {
		console_puts("type: file\r\n");
	} else if (st.type == KFS_TYPE_DIR) {
		console_puts("type: dir\r\n");
	} else {
		printf("type: %u\r\n", (unsigned int)st.type);
	}
	printf("attributes: %02x\r\n", (unsigned int)st.attributes);
	printf("size: %lu\r\n", st.size);
	printf("mtime: %08lx\r\n", st.mtime);
	return 0;
}

static int fscmd_stat(int argc, char **argv)
{
	int status = 0;
	int index;

	if (argc < 2) {
		return fscmd_usage("stat path ...");
	}
	for (index = 1; index < argc; ++index) {
		int result = fscmd_translate("stat", argv[index], fscmd_src);

		if (result != 0) {
			status = result;
			continue;
		}
		result = fscmd_stat_one(fscmd_src);
		if (result != 0) {
			status = result;
		}
	}
	return status;
}

/* --- cp and mv ---------------------------------------------------------- */

/* The target of `cp`/`mv`: `dst` itself, or the name below it when
 * `dst` is the directory to copy into.  A trailing slash says so even
 * when nothing is there to `stat` yet. */
static int fscmd_target_of(const char *src, const char *dst, char *out)
{
	struct kfs_stat st;
	int directory = 0;

	if (stat(dst, &st) == 0 && st.type == KFS_TYPE_DIR) {
		directory = 1;
	}
	return shell_fscmd_target(src, dst, directory, out, KOS_PATH_MAX);
}

/* Copy `src` to the translated `target` through io_buf, then flush and
 * release both handles.  A failed transfer (read, write or flush)
 * removes the partial target and reports that error
 * (requirement 5). */
static int fscmd_copy_data(const char *command, const char *src,
			   const char *target)
{
	int in = open(src, KFS_O_READ);
	int out;
	int fail = 0;

	if (in < 0) {
		return fscmd_error(command, src, -in);
	}
	out = open(target, KFS_O_WRITE | KFS_O_CREATE | KFS_O_TRUNC);
	if (out < 0) {
		(void)close((unsigned int)in);
		return fscmd_error(command, target, -out);
	}
	for (;;) {
		int count = read((unsigned int)in, io_buf, sizeof io_buf);
		int done = 0;

		if (count < 0) {
			fail = -count;
			break;
		}
		if (count == 0) {
			break;                     /* end of file */
		}
		/* A write may complete in part: the position advanced
		 * with it, so the rest goes out as the next chunk. */
		while (done < count) {
			int written = write((unsigned int)out, &io_buf[done],
					    (unsigned int)(count - done));

			if (written < 0) {
				fail = -written;
				break;
			}
			if (written == 0) {
				fail = KOS_EIO;    /* no progress */
				break;
			}
			done += written;
		}
		if (fail != 0) {
			break;
		}
	}
	if (fail == 0) {
		int status = sync((unsigned int)out);

		if (status < 0) {
			fail = -status;
		}
	}
	(void)close((unsigned int)in);
	(void)close((unsigned int)out);
	if (fail == 0) {
		return 0;
	}
	(void)unlink(target);
	return fscmd_error(command, target, fail);
}

static int fscmd_cp(int argc, char **argv)
{
	struct kfs_stat st;
	int status;

	if (argc != 3) {
		return fscmd_usage("cp src dst");
	}
	status = fscmd_translate("cp", argv[1], fscmd_src);
	if (status == 0) {
		status = fscmd_translate("cp", argv[2], fscmd_dst);
	}
	if (status != 0) {
		return status;
	}
	if (stat(fscmd_src, &st) == 0 && st.type == KFS_TYPE_DIR) {
		return fscmd_error("cp", fscmd_src, KOS_EISDIR);
	}
	if (fscmd_target_of(fscmd_src, fscmd_dst, fscmd_target) < 0) {
		return fscmd_error("cp", fscmd_dst, KOS_EINVAL);
	}
	return fscmd_copy_data("cp", fscmd_src, fscmd_target);
}

/* The reserved root a translated path sits under, or NULL when it names
 * none: a relative path takes the CWD's root, and one that climbs above
 * the CWD is the namespace's business - its rename answer stands. */
static const char *fscmd_root_of(const char *path)
{
	if (path[0] != '/') {
		if (getcwd(fscmd_cwd, sizeof fscmd_cwd) < 0) {
			return (const char *)0;
		}
		return shell_path_root(fscmd_cwd);
	}
	return shell_path_root(path);
}

static int fscmd_mv(int argc, char **argv)
{
	int status;

	if (argc != 3) {
		return fscmd_usage("mv src dst");
	}
	status = fscmd_translate("mv", argv[1], fscmd_src);
	if (status == 0) {
		status = fscmd_translate("mv", argv[2], fscmd_dst);
	}
	if (status != 0) {
		return status;
	}
	if (fscmd_target_of(fscmd_src, fscmd_dst, fscmd_target) < 0) {
		return fscmd_error("mv", fscmd_dst, KOS_EINVAL);
	}
	if (fscmd_root_of(fscmd_src) == fscmd_root_of(fscmd_dst)) {
		status = rename(fscmd_src, fscmd_target);
		if (status == 0) {
			return 0;
		}
		if (status != -KOS_EXDEV) {
			return fscmd_error("mv", fscmd_src, -status);
		}
	}
	/* Across providers the system never renames: copy, then drop the
	 * source.  A failed unlink keeps the copy and is the status
	 * (requirement 6). */
	status = fscmd_copy_data("mv", fscmd_src, fscmd_target);
	if (status != 0) {
		return status;
	}
	status = unlink(fscmd_src);
	if (status < 0) {
		return fscmd_error("mv", fscmd_src, -status);
	}
	return 0;
}

/* --- rm, mkdir and rmdir ------------------------------------------------ */

/* One leaf call on a translated path. */
static int fscmd_leaf(int operation, const char *path)
{
	if (operation == FSCMD_UNLINK) {
		return unlink(path);
	}
	if (operation == FSCMD_MKDIR) {
		return mkdir(path);
	}
	return rmdir(path);
}

/* One call per argument, each error reported before the next one; the
 * status is the last error, or 0 (requirement 7). */
static int fscmd_leaves(const char *command, int operation, int argc,
			char **argv)
{
	int status = 0;
	int index;

	for (index = 1; index < argc; ++index) {
		int result = fscmd_translate(command, argv[index], fscmd_src);

		if (result != 0) {
			status = result;
			continue;
		}
		result = fscmd_leaf(operation, fscmd_src);
		if (result < 0) {
			status = fscmd_error(command, fscmd_src, -result);
		}
	}
	return status;
}

static int fscmd_rm(int argc, char **argv)
{
	if (argc < 2) {
		return fscmd_usage("rm file ...");
	}
	return fscmd_leaves("rm", FSCMD_UNLINK, argc, argv);
}

static int fscmd_mkdir(int argc, char **argv)
{
	if (argc < 2) {
		return fscmd_usage("mkdir dir ...");
	}
	return fscmd_leaves("mkdir", FSCMD_MKDIR, argc, argv);
}

static int fscmd_rmdir(int argc, char **argv)
{
	if (argc < 2) {
		return fscmd_usage("rmdir dir ...");
	}
	return fscmd_leaves("rmdir", FSCMD_RMDIR, argc, argv);
}

/* --- the target rule, the dispatcher ------------------------------------ */

int shell_fscmd_target(const char *src, const char *dst, int directory,
		       char *out, unsigned int cap)
{
	unsigned int length = strlen(dst);
	unsigned int prefix = length;
	unsigned int needed;
	const char *name;

	if (directory == 0 && (length == 0u || dst[length - 1u] != '/')) {
		/* `dst` is the target itself. */
		if (length + 1u > cap) {
			return -KOS_EINVAL;
		}
		(void)strlcpy(out, dst, cap);
		return 0;
	}
	/* `dst` is the directory: the target is `dst` and the source's own
	 * last component. */
	if (prefix > 0u && dst[prefix - 1u] == '/') {
		--prefix;                      /* the join adds it back */
	}
	name = fscmd_basename(src);
	needed = prefix + 1u + strlen(name);
	if (needed + 1u > cap) {
		return -KOS_EINVAL;
	}
	(void)strlcpy(out, dst, cap);
	out[prefix] = '\0';
	(void)strlcat(out, "/", cap);
	(void)strlcat(out, name, cap);
	return 0;
}

int shell_file_command(int argc, char **argv)
{
	if (strcmp(argv[0], "ls") == 0) {
		return fscmd_ls(argc, argv);
	}
	if (strcmp(argv[0], "cat") == 0) {
		return fscmd_cat(argc, argv);
	}
	if (strcmp(argv[0], "stat") == 0) {
		return fscmd_stat(argc, argv);
	}
	if (strcmp(argv[0], "cp") == 0) {
		return fscmd_cp(argc, argv);
	}
	if (strcmp(argv[0], "mv") == 0) {
		return fscmd_mv(argc, argv);
	}
	if (strcmp(argv[0], "rm") == 0) {
		return fscmd_rm(argc, argv);
	}
	if (strcmp(argv[0], "mkdir") == 0) {
		return fscmd_mkdir(argc, argv);
	}
	if (strcmp(argv[0], "rmdir") == 0) {
		return fscmd_rmdir(argc, argv);
	}
	return -1;
}
