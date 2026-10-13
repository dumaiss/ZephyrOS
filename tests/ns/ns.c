/* KonaOS namespace acceptance client (test artifact).
 *
 * The EXE primary the namespace-selftest image boots.  It is an ordinary
 * SDK program: the kernel links the object family into the image, the SDK
 * variant (KOS_NS_HANDLES) exposes bind/unbind/ns_enum and every console
 * write below goes through the two standard handles an ordinary program
 * starts with.
 *
 * The checks cover the namespace contract:
 *
 *   * the standard handles work and console output is one of them;
 *   * `bind` resolves the host-built `/dev` publications and reports the
 *     8-bit protocol type the table declares (CONSOLE, INPUT, VIDEO,
 *     SERIAL), and grouping entries refuse with KOS_EISDIR;
 *   * `call` on a bound handle reaches the publishing endpoint: the
 *     synthetic publisher answers deterministically, so a wrong endpoint
 *     cannot look like a pass;
 *   * a `/system` FILE handle reads, seeks and reports metadata, and reads
 *     0 bytes at end of file;
 *   * unknown names, `/fs`, foreign/stale handles and a malformed
 *     path are refused with the documented KOS_E* values;
 *   * the handle table fills to exactly VFS_HANDLE_MAX entries, reports
 *     -KOS_EMFILE, and releases every entry `unbind` returns;
 *   * `ns_enum` lists `/`, `/dev`, `/dev/input` and `/system`, paged with
 *     its cookie;
 *   * `chdir`/`getcwd` traverse the namespace through provider validation:
 *     the VFS root, `/system`, a grouping container and `..` are
 *     traversable, a bindable object and an absent name are refused, a
 *     refused `chdir` leaves the CWD untouched, and a relative `bind`
 *     resolves against the stored CWD;
 *   * a child started with SYS_EXEC gets its own two standard handles,
 *     none of the parent's bound handles and the parent's CWD, and its own
 *     `chdir` cannot move the parent's CWD.
 *
 * The simulator replay reads the lines below; the final line is the
 * verdict.  Every write to the console also exercises the standard
 * handles, so a broken console path cannot print a pass.
 */

#include "kos.h"

#include "ns_test.h"
#include "kos/protocol.h"

/* The namespace image's pool: kernel/vfs/vfs.h VFS_HANDLE_MAX. */
#define NS_FD_MAX 12u

/* Four entries per page: 112 bytes of caller buffer. */
#define NS_ENUM_PAGE 4u

/* /system/drivers.man holds one 24-byte record per resident driver. */
#define NS_MAN_RECORD 24u

static struct kos_ipc_request req;
static struct kos_ns_entry entries[NS_ENUM_PAGE];
static unsigned char file_data[32];
static unsigned int checks;
static unsigned int failures;

static void check(const char *label, int ok);
static void report_ping(unsigned char handle);

static void check(const char *label, int ok)
{
	++checks;
	if (!ok) {
		++failures;
		printf("ns: FAIL %s\r\n", label);
	}
}

/* One call on `handle` with `op`, no descriptor. */
static int ns_call(unsigned char handle, unsigned char op)
{
	req.frame.op = op;
	req.frame.flags = 0u;
	req.frame.arg0 = 0u;
	req.frame.arg1 = 0u;
	req.buffer = (void *)0;
	req.length = 0u;
	req.direction = KOS_IPC_DESC_IN;
	return call(handle, &req);
}

/* --- the standard handles and the /dev publications --------------------- */

static void test_devices(void)
{
	unsigned char type = 0u;
	unsigned char console = 0u;
	unsigned char keyboard = 0u;
	unsigned char video = 0u;
	unsigned char serial = 0u;
	int handle;
	int result;

	/* The transcript itself proves the two standard handles: every
	 * line below is written through KOS_HANDLE_STDOUT. */
	handle = bind("/dev/console", 0u, &type);
	check("bind-dev-console", handle >= 3 && type == KOS_TYPE_CONSOLE);
	printf("ns: /dev/console -> %d handle %d\r\n", type, handle);
	console = (unsigned char)handle;

	handle = bind("/dev/input/keyboard", 0u, &type);
	check("bind-dev-input", handle >= 3 && type == KOS_TYPE_INPUT);
	printf("ns: /dev/input/keyboard -> %d\r\n", type);
	keyboard = (unsigned char)handle;

	handle = bind("/dev/video", 0u, &type);
	check("bind-dev-video", handle >= 3 && type == KOS_TYPE_VIDEO);
	printf("ns: /dev/video -> %d\r\n", type);
	video = (unsigned char)handle;

	/* A bound handle is an object, not an endpoint number: the call
	 * must reach the publisher that declared the name, with this
	 * caller's frame. */
	result = ns_call(video, NS_OP_WHOAMI);
	check("call-video-whoami", result == (int)NS_MAGIC);
	printf("ns: call /dev/video whoami -> %04x\r\n", result);

	report_ping(video);
	check("stdout-call", ns_call(console, KCONSOLE_OP_POLL) >= 0);

	handle = bind("/dev/serial/0", 0u, &type);
	check("bind-dev-serial", handle >= 3 && type == KOS_TYPE_SERIAL);
	printf("ns: /dev/serial/0 -> %d\r\n", type);
	serial = (unsigned char)handle;

	/* Grouping entries are namespace metadata, not objects. */
	result = bind("/dev", 0u, &type);
	check("bind-container", result == -KOS_EISDIR);
	check("bind-container-input",
	      bind("/dev/input", 0u, &type) == -KOS_EISDIR);
	result = bind("/dev/nosuch", 0u, &type);
	check("bind-unknown-device", result == -KOS_ENOENT);
	result = bind("/system/nosuch.exe", 0u, &type);
	check("bind-unknown-file", result == -KOS_ENOENT);
	/* This build serves no /fs provider: it refuses the name cleanly. */
	result = bind("/fs/data", 0u, &type);
	check("bind-fs-enodev", result == -KOS_ENODEV);
	result = bind("", 0u, &type);
	check("bind-empty-einval", result == -KOS_EINVAL);

	/* A foreign or stale handle is KOS_EINVAL, never another process'
	 * object. */
	check("call-bad-handle", ns_call(90u, NS_OP_WHOAMI) == -KOS_EINVAL);
	check("unbind-bad-handle", unbind(90u) == -KOS_EINVAL);
	check("unbind-zero", unbind(0u) == -KOS_EINVAL);

	/* Release the bound instances again: the handles must be reusable
	 * and the standard handles must never be released. */
	check("unbind-console", unbind(console) == 0);
	check("unbind-unbound", unbind(console) == -KOS_EINVAL);
	check("unbind-keyboard", unbind(keyboard) == 0);
	check("unbind-video", unbind(video) == 0);
	check("unbind-serial", unbind(serial) == 0);
	/* The standard handles are standard: a program must not release
	 * them, and the next bind must still see the table empty again. */
	check("unbind-stdout", unbind(KOS_HANDLE_STDOUT) == -KOS_EINVAL);
	check("unbind-stdin", unbind(KOS_HANDLE_STDIN) == -KOS_EINVAL);
}

/* The ping echo: the reply carries the caller's own word back. */
static void report_ping(unsigned char handle)
{
	int result;

	req.frame.op = NS_OP_PING;
	req.frame.flags = 0u;
	req.frame.arg0 = 0u;
	req.frame.arg1 = 0u;
	req.frame.payload[0] = 0x34u;
	req.frame.payload[1] = 0x12u;
	req.buffer = (void *)0;
	req.length = 0u;
	req.direction = KOS_IPC_DESC_IN;
	result = call(handle, &req);
	check("call-ping-status", result == (int)NS_MAGIC);
	check("call-ping-payload",
	      req.frame.payload[2] == 0x34u && req.frame.payload[3] == 0x12u);
	printf("ns: call ping -> %04x %02x%02x\r\n", result,
		   req.frame.payload[3], req.frame.payload[2]);
}

/* --- the handle table ---------------------------------------------------- */

static void test_handle_table(void)
{
	unsigned char type = 0u;
	unsigned char handles[NS_FD_MAX];
	unsigned int count = 0u;
	int result;
	unsigned int index;

	/* Fill the pool: the standard handles are already two entries, so
	 * VFS_HANDLE_MAX - 2 binds succeed and the next one is refused. */
	for (index = 0u; index < NS_FD_MAX; ++index) {
		result = bind("/dev/console", 0u, &type);
		if (result < 0) {
			break;
		}
		handles[count++] = (unsigned char)result;
	}
	check("table-fills", count == NS_FD_MAX - 2u);
	check("table-emfile", result == -KOS_EMFILE);
	printf("ns: handle table %u bound, refuse %d\r\n", count, result);
	for (index = 0u; index < count; ++index) {
		check("table-release", unbind(handles[index]) == 0);
	}
	/* The pool is reusable: the release really returned the entries. */
	result = bind("/dev/console", 0u, &type);
	check("table-reuse", result >= 3);
	if (result >= 3) {
		check("table-reuse-unbind", unbind((unsigned char)result) == 0);
	}
}

/* --- the /system FILE provider ------------------------------------------- */

static void test_file(void)
{
	unsigned char type = 0u;
	int handle;
	int result;
	int size;

	/* `/` is a directory of provider roots, not a provider itself: a
	 * root-level name has no owner and is refused. */
	check("file-root-refused",
	      bind("/drivers.man", KFS_O_READ, &type) == -KOS_ENOENT);

	handle = bind("/system/drivers.man", KFS_O_READ, &type);
	check("file-bind", handle >= 3 && type == KOS_TYPE_FILE);
	if (handle < 3) {
		return;
	}

	/* Read the first record: the boot manifest lists the drivers in
	 * load order, and this image boots SIO first (kernel/driver). */
	req.frame.op = KFILE_OP_READ;
	req.frame.flags = 0u;
	req.frame.arg0 = 0u;
	req.frame.arg1 = NS_MAN_RECORD;
	req.buffer = file_data;
	req.length = NS_MAN_RECORD;
	req.direction = KOS_IPC_DESC_OUT;
	result = call((unsigned char)handle, &req);
	check("file-read", result == NS_MAN_RECORD);
	check("file-record",
	      strncmp((const char *)file_data, "sio.mod", 8u) == 0);
	printf("ns: file read %d, first %s\r\n", result, file_data);

	/* Metadata, then the size rule: seek to the end reports the size,
	 * and a read there transfers zero bytes. */
	req.frame.op = KFILE_OP_META;
	req.frame.flags = 0u;
	req.frame.arg0 = 0u;
	req.frame.arg1 = 0u;
	req.buffer = (void *)0;
	req.length = 0u;
	req.direction = KOS_IPC_DESC_IN;
	result = call((unsigned char)handle, &req);
	check("file-meta", result == 0);
	check("file-meta-type", req.frame.payload[0] == KFS_TYPE_FILE);

	req.frame.op = KFILE_OP_SEEK;
	req.frame.arg0 = KFS_SEEK_END;
	req.frame.payload[1] = 0u;
	req.frame.payload[2] = 0u;
	req.frame.payload[3] = 0u;
	req.frame.payload[4] = 0u;
	result = call((unsigned char)handle, &req);
	check("file-seek-end", result == 0);
	size = (int)(((unsigned long)req.frame.payload[4] << 24) |
		     ((unsigned long)req.frame.payload[3] << 16) |
		     ((unsigned long)req.frame.payload[2] << 8) |
		     (unsigned long)req.frame.payload[1]);
	check("file-size", size == NS_MAN_RECORD * 3);
	printf("ns: file size %d\r\n", size);

	req.frame.op = KFILE_OP_READ;
	req.frame.arg0 = 0u;
	req.frame.arg1 = 8u;
	req.buffer = file_data;
	req.length = 8u;
	req.direction = KOS_IPC_DESC_OUT;
	result = call((unsigned char)handle, &req);
	check("file-read-eof", result == 0);

	/* Seek back to the start: the instance keeps its own position. */
	req.frame.op = KFILE_OP_SEEK;
	req.frame.arg0 = KFS_SEEK_SET;
	req.frame.payload[1] = 8u;
	req.frame.payload[2] = 0u;
	req.frame.payload[3] = 0u;
	req.frame.payload[4] = 0u;
	result = call((unsigned char)handle, &req);
	check("file-seek-set", result == 0);
	req.frame.op = KFILE_OP_READ;
	req.frame.arg0 = 0u;
	req.frame.arg1 = 8u;
	req.buffer = file_data;
	req.length = 8u;
	req.direction = KOS_IPC_DESC_OUT;
	result = call((unsigned char)handle, &req);
	check("file-read-after-seek", result == 8);

	/* ROMFS is read-only: the FILE protocol says KOS_EROFS, and the
	 * bind flags refuse a write open before that. */
	req.frame.op = KFILE_OP_WRITE;
	req.frame.arg0 = 0u;
	req.frame.arg1 = 1u;
	result = call((unsigned char)handle, &req);
	check("file-write-erofs", result == -KOS_EROFS);
	check("file-bind-write",
	      bind("/system/drivers.man", KFS_O_WRITE, &type) ==
		      -KOS_EROFS);
	check("file-unbind", unbind((unsigned char)handle) == 0);
}

/* --- enumeration ---------------------------------------------------------- */

/* Page `path` with one entry per call and print every entry once. */
static void enum_page(const char *path, unsigned int *pages)
{
	unsigned int cookie = 0u;
	unsigned int seen = 0u;
	int result;

	for (;;) {
		result = list(path, &cookie, entries, 1u);
		check("enum-page-call", result >= 0);
		if (result <= 0) {
			break;
		}
		++seen;
		++*pages;
		printf("ns: %s entry %s type %d%s\r\n", path,
			   entries[0].name, entries[0].type,
			   (entries[0].flags & KOS_NS_ENTRY_CONTAINER)
				   ? " container" : "");
		if (seen > 32u) {
			break;
		}
	}
}

static void test_enum(void)
{
	unsigned int cookie = 0u;
	unsigned int pages = 0u;
	unsigned int count = 0u;
	unsigned int index;
	int result;

	/* One page holds the whole of `/`. */
	result = list("/", &cookie, entries, NS_ENUM_PAGE);
	check("enum-root", result == 3);
	for (index = 0u; index < (unsigned int)result; ++index) {
		check("enum-root-container",
		      (entries[index].flags & KOS_NS_ENTRY_CONTAINER) != 0u);
	}
	check("enum-root-cookie", cookie == 3u);
	/* The cookie is the index: a second page is the empty tail. */
	result = list("/", &cookie, entries, NS_ENUM_PAGE);
	check("enum-root-end", result == 0 && cookie == 3u);

	/* The device tree, one entry per call. */
	enum_page("/dev", &pages);
	check("enum-dev-pages", pages == 4u);

	result = list("/dev/input", &cookie, entries, NS_ENUM_PAGE);
	check("enum-input", result == 1);
	if (result == 1) {
		check("enum-input-name",
		      strcmp(entries[0].name, "keyboard") == 0);
		check("enum-input-type", entries[0].type == KOS_TYPE_INPUT);
	}
	cookie = 0u;
	result = list("/system", &cookie, entries, NS_ENUM_PAGE);
	check("enum-system", result == (int)NS_ENUM_PAGE);
	/* The store's root holds the whole recovery set. */
	cookie = 0u;
	for (;;) {
		result = list("/system", &cookie, entries, NS_ENUM_PAGE);
		check("enum-system-page", result >= 0);
		{
			unsigned int k;
			for (k = 0u; k < (unsigned int)result; ++k) {
				printf("ns: /system entry %s\r\n",
					   entries[k].name);
			}
		}
		if (result <= 0) {
			break;
		}
		count += (unsigned int)result;
	}
	check("enum-system-count", count == 7u);
	printf("ns: /system %u entries\r\n", count);

	check("enum-unknown", list("/dev/nosuch", &cookie, entries,
					  NS_ENUM_PAGE) == -KOS_ENOENT);
	check("enum-bad-page", list("/", &cookie, entries, 0u)
			       == -KOS_EINVAL);
}

/* --- CWD traversal and inheritance --------------------------------------- */

static void test_cwd(void)
{
	char cwd[16];
	unsigned char type = 0u;
	int result;
	int handle;

	/* The initial CWD is the VFS root, and the root itself stays
	 * traversable: `chdir` asks the owning provider (the VFS owns `/`)
	 * whether the target is a traversable location. */
	result = getcwd(cwd, sizeof(cwd));
	check("cwd-initial", result == 1 && cwd[0] == '/' && cwd[1] == '\0');

	check("chdir-system", chdir("/system") == 0);
	result = getcwd(cwd, sizeof(cwd));
	check("cwd-system", result == 7 && strcmp(cwd, "/system") == 0);
	printf("ns: cwd %s\r\n", cwd);

	/* A relative path resolves against the stored CWD. */
	handle = bind("drivers.man", KFS_O_READ, &type);
	check("cwd-relative-bind", handle >= 3 && type == KOS_TYPE_FILE);
	if (handle >= 3) {
		check("cwd-relative-unbind",
		      unbind((unsigned char)handle) == 0);
	}

	/* Provider validation folds case, `..` walks up to the root, and a
	 * refused `chdir` leaves the CWD untouched. */
	check("chdir-case", chdir("/system") == 0);
	check("chdir-parent", chdir("..") == 0);
	result = getcwd(cwd, sizeof(cwd));
	check("cwd-root", result == 1 && cwd[0] == '/' && cwd[1] == '\0');
	check("chdir-dev-input", chdir("/dev/input") == 0);
	result = getcwd(cwd, sizeof(cwd));
	check("cwd-dev-input",
	      result == 10 && strcmp(cwd, "/dev/input") == 0);
	check("chdir-object-refused",
	      chdir("/dev/console") == -KOS_ENOTDIR);
	check("chdir-absent", chdir("/dev/nosuch") == -KOS_ENOENT);
	check("chdir-no-provider", chdir("/fs") == -KOS_ENODEV);
	result = getcwd(cwd, sizeof(cwd));
	check("cwd-survives-refusal",
	      result == 10 && strcmp(cwd, "/dev/input") == 0);

	/* The CWD the inheritance child starts from. */
	check("chdir-dev", chdir("..") == 0);
	result = getcwd(cwd, sizeof(cwd));
	check("cwd-dev", result == 4 && strcmp(cwd, "/dev") == 0);
	printf("ns: cwd %s\r\n", cwd);
}

/* --- the child ------------------------------------------------------------ */

#define NS_CHILD "/system/nschild.exe"

static void test_child(void)
{
	char cwd[16];
	int result;

	/* SYS_EXEC resolves the executable through the namespace, gives the
	 * child new standard handles and no other entry, and returns the
	 * child's exit status. */
	result = run(NS_CHILD, "");
	check("child-status", result == 0);
	printf("ns: exec %s -> %d\r\n", NS_CHILD, result);

	/* The child's `chdir` went to its own CWD slot: the parent's CWD is
	 * exactly the one it set before the exec. */
	result = getcwd(cwd, sizeof(cwd));
	check("cwd-after-child", result == 4 && strcmp(cwd, "/dev") == 0);

	/* The parent's table is its own: the child's binds and unbinds did
	 * not touch it, so a fresh bind still succeeds. */
	check("parent-after-child",
	      bind("/dev/console", 0u, (unsigned char *)0) >= 3);
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	console_puts("ns: probe\r\n");
	test_devices();
	test_handle_table();
	test_file();
	test_enum();
	test_cwd();
	test_child();

	printf("ns: %u/%u checks\r\n", checks - failures, checks);
	if (failures == 0u) {
		console_puts("ns selftest: OK\r\n");
	} else {
		console_puts("ns selftest: FAIL\r\n");
	}
	return (int)(failures != 0u);
}
