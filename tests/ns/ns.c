/* ZephyrOS namespace acceptance client (work package 42a, test artifact).
 *
 * The ZEX primary the namespace-selftest image boots.  It is an ordinary
 * SDK program: the kernel links the object family into the image, the SDK
 * variant (ZOS_NS_HANDLES) exposes bind/unbind/ns_enum and every console
 * write below goes through the two standard handles an ordinary program
 * starts with.
 *
 * The checks cover the contract of 42a:
 *
 *   * the standard handles work and console output is one of them;
 *   * `bind` resolves the host-built `/dev` publications and reports the
 *     8-bit protocol type the table declares (CONSOLE, INPUT, VIDEO,
 *     SERIAL), and grouping entries refuse with ZOS_EISDIR;
 *   * `call` on a bound handle reaches the publishing endpoint: the
 *     synthetic publisher answers deterministically, so a wrong endpoint
 *     cannot look like a pass;
 *   * a `/SYSTEM` FILE handle reads, seeks and reports metadata, and reads
 *     0 bytes at end of file;
 *   * unknown names, `/FS` (42b), foreign/stale handles and a malformed
 *     path are refused with the documented ZOS_E* values;
 *   * the handle table fills to exactly VFS_FD_MAX entries, reports
 *     -ZOS_EMFILE, and releases every entry `unbind` returns;
 *   * `ns_enum` lists `/`, `/dev`, `/dev/input` and `/SYSTEM`, paged with
 *     its cookie;
 *   * `chdir`/`getcwd` traverse the namespace through provider validation:
 *     the VFS root, `/SYSTEM`, a grouping container and `..` are
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

#include "zos.h"

#include "ns_test.h"
#include "zos/protocol.h"

/* The namespace image's pool: kernel/vfs/vfs.h VFS_FD_MAX. */
#define NS_FD_MAX 12u

/* Four entries per page: 112 bytes of caller buffer. */
#define NS_ENUM_PAGE 4u

/* /SYSTEM/services.man holds one 24-byte record per resident service. */
#define NS_MAN_RECORD 24u

static struct zos_ipc_request req;
static struct zos_ns_entry entries[NS_ENUM_PAGE];
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
		zos_printf("ns: FAIL %s\r\n", label);
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
	req.direction = ZOS_IPC_DESC_IN;
	return zos_ipc_call(handle, &req);
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
	 * line below is written through ZOS_HANDLE_STDOUT. */
	handle = zos_ipc_bind("/dev/console", 0u, &type);
	check("bind-dev-console", handle >= 3 && type == ZOS_TYPE_CONSOLE);
	zos_printf("ns: /dev/console -> %d handle %d\r\n", type, handle);
	console = (unsigned char)handle;

	handle = zos_ipc_bind("/dev/input/keyboard", 0u, &type);
	check("bind-dev-input", handle >= 3 && type == ZOS_TYPE_INPUT);
	zos_printf("ns: /dev/input/keyboard -> %d\r\n", type);
	keyboard = (unsigned char)handle;

	handle = zos_ipc_bind("/dev/video", 0u, &type);
	check("bind-dev-video", handle >= 3 && type == ZOS_TYPE_VIDEO);
	zos_printf("ns: /dev/video -> %d\r\n", type);
	video = (unsigned char)handle;

	/* A bound handle is an object, not an endpoint number: the call
	 * must reach the publisher that declared the name, with this
	 * caller's frame. */
	result = ns_call(video, NS_OP_WHOAMI);
	check("call-video-whoami", result == (int)NS_MAGIC);
	zos_printf("ns: call /dev/video whoami -> %04x\r\n", result);

	report_ping(video);
	check("stdout-call", ns_call(console, ZCONSOLE_OP_POLL) >= 0);

	handle = zos_ipc_bind("/dev/serial/0", 0u, &type);
	check("bind-dev-serial", handle >= 3 && type == ZOS_TYPE_SERIAL);
	zos_printf("ns: /dev/serial/0 -> %d\r\n", type);
	serial = (unsigned char)handle;

	/* Grouping entries are namespace metadata, not objects. */
	result = zos_ipc_bind("/dev", 0u, &type);
	check("bind-container", result == -ZOS_EISDIR);
	check("bind-container-input",
	      zos_ipc_bind("/dev/input", 0u, &type) == -ZOS_EISDIR);
	result = zos_ipc_bind("/dev/nosuch", 0u, &type);
	check("bind-unknown-device", result == -ZOS_ENOENT);
	result = zos_ipc_bind("/SYSTEM/nosuch.zex", 0u, &type);
	check("bind-unknown-file", result == -ZOS_ENOENT);
	/* /FS is package 42b's provider: this build refuses it cleanly. */
	result = zos_ipc_bind("/FS/data", 0u, &type);
	check("bind-fs-enodev", result == -ZOS_ENODEV);
	result = zos_ipc_bind("", 0u, &type);
	check("bind-empty-einval", result == -ZOS_EINVAL);

	/* A foreign or stale handle is ZOS_EINVAL, never another process'
	 * object. */
	check("call-bad-handle", ns_call(90u, NS_OP_WHOAMI) == -ZOS_EINVAL);
	check("unbind-bad-handle", zos_ipc_unbind(90u) == -ZOS_EINVAL);
	check("unbind-zero", zos_ipc_unbind(0u) == -ZOS_EINVAL);

	/* Release the bound instances again: the handles must be reusable
	 * and the standard handles must never be released. */
	check("unbind-console", zos_ipc_unbind(console) == 0);
	check("unbind-unbound", zos_ipc_unbind(console) == -ZOS_EINVAL);
	check("unbind-keyboard", zos_ipc_unbind(keyboard) == 0);
	check("unbind-video", zos_ipc_unbind(video) == 0);
	check("unbind-serial", zos_ipc_unbind(serial) == 0);
	/* The standard handles are standard: a program must not release
	 * them, and the next bind must still see the table empty again. */
	check("unbind-stdout", zos_ipc_unbind(ZOS_HANDLE_STDOUT) == -ZOS_EINVAL);
	check("unbind-stdin", zos_ipc_unbind(ZOS_HANDLE_STDIN) == -ZOS_EINVAL);
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
	req.direction = ZOS_IPC_DESC_IN;
	result = zos_ipc_call(handle, &req);
	check("call-ping-status", result == (int)NS_MAGIC);
	check("call-ping-payload",
	      req.frame.payload[2] == 0x34u && req.frame.payload[3] == 0x12u);
	zos_printf("ns: call ping -> %04x %02x%02x\r\n", result,
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
	 * VFS_FD_MAX - 2 binds succeed and the next one is refused. */
	for (index = 0u; index < NS_FD_MAX; ++index) {
		result = zos_ipc_bind("/dev/console", 0u, &type);
		if (result < 0) {
			break;
		}
		handles[count++] = (unsigned char)result;
	}
	check("table-fills", count == NS_FD_MAX - 2u);
	check("table-emfile", result == -ZOS_EMFILE);
	zos_printf("ns: handle table %u bound, refuse %d\r\n", count, result);
	for (index = 0u; index < count; ++index) {
		check("table-release", zos_ipc_unbind(handles[index]) == 0);
	}
	/* The pool is reusable: the release really returned the entries. */
	result = zos_ipc_bind("/dev/console", 0u, &type);
	check("table-reuse", result >= 3);
	if (result >= 3) {
		check("table-reuse-unbind", zos_ipc_unbind((unsigned char)result) == 0);
	}
}

/* --- the /SYSTEM FILE provider ------------------------------------------- */

static void test_file(void)
{
	unsigned char type = 0u;
	int handle;
	int result;
	int size;

	/* `/` is a directory of provider roots, not a provider itself: a
	 * root-level name has no owner and is refused. */
	check("file-root-refused",
	      zos_ipc_bind("/services.man", ZFS_O_READ, &type) == -ZOS_ENOENT);

	handle = zos_ipc_bind("/SYSTEM/services.man", ZFS_O_READ, &type);
	check("file-bind", handle >= 3 && type == ZOS_TYPE_FILE);
	if (handle < 3) {
		return;
	}

	/* Read the first record: the boot manifest lists the services in
	 * load order, and this image boots SIO first (kernel/service). */
	req.frame.op = ZFILE_OP_READ;
	req.frame.flags = 0u;
	req.frame.arg0 = 0u;
	req.frame.arg1 = NS_MAN_RECORD;
	req.buffer = file_data;
	req.length = NS_MAN_RECORD;
	req.direction = ZOS_IPC_DESC_OUT;
	result = zos_ipc_call((unsigned char)handle, &req);
	check("file-read", result == NS_MAN_RECORD);
	check("file-record",
	      strncmp((const char *)file_data, "sio.zmod", 8u) == 0);
	zos_printf("ns: file read %d, first %s\r\n", result, file_data);

	/* Metadata, then the size rule: seek to the end reports the size,
	 * and a read there transfers zero bytes. */
	req.frame.op = ZFILE_OP_META;
	req.frame.flags = 0u;
	req.frame.arg0 = 0u;
	req.frame.arg1 = 0u;
	req.buffer = (void *)0;
	req.length = 0u;
	req.direction = ZOS_IPC_DESC_IN;
	result = zos_ipc_call((unsigned char)handle, &req);
	check("file-meta", result == 0);
	check("file-meta-type", req.frame.payload[0] == ZFS_TYPE_FILE);

	req.frame.op = ZFILE_OP_SEEK;
	req.frame.arg0 = ZFS_SEEK_END;
	req.frame.payload[1] = 0u;
	req.frame.payload[2] = 0u;
	req.frame.payload[3] = 0u;
	req.frame.payload[4] = 0u;
	result = zos_ipc_call((unsigned char)handle, &req);
	check("file-seek-end", result == 0);
	size = (int)(((unsigned long)req.frame.payload[4] << 24) |
		     ((unsigned long)req.frame.payload[3] << 16) |
		     ((unsigned long)req.frame.payload[2] << 8) |
		     (unsigned long)req.frame.payload[1]);
	check("file-size", size == NS_MAN_RECORD * 3);
	zos_printf("ns: file size %d\r\n", size);

	req.frame.op = ZFILE_OP_READ;
	req.frame.arg0 = 0u;
	req.frame.arg1 = 8u;
	req.buffer = file_data;
	req.length = 8u;
	req.direction = ZOS_IPC_DESC_OUT;
	result = zos_ipc_call((unsigned char)handle, &req);
	check("file-read-eof", result == 0);

	/* Seek back to the start: the instance keeps its own position. */
	req.frame.op = ZFILE_OP_SEEK;
	req.frame.arg0 = ZFS_SEEK_SET;
	req.frame.payload[1] = 8u;
	req.frame.payload[2] = 0u;
	req.frame.payload[3] = 0u;
	req.frame.payload[4] = 0u;
	result = zos_ipc_call((unsigned char)handle, &req);
	check("file-seek-set", result == 0);
	req.frame.op = ZFILE_OP_READ;
	req.frame.arg0 = 0u;
	req.frame.arg1 = 8u;
	req.buffer = file_data;
	req.length = 8u;
	req.direction = ZOS_IPC_DESC_OUT;
	result = zos_ipc_call((unsigned char)handle, &req);
	check("file-read-after-seek", result == 8);

	/* ROMFS is read-only: the FILE protocol says ZOS_EROFS, and the
	 * bind flags refuse a write open before that. */
	req.frame.op = ZFILE_OP_WRITE;
	req.frame.arg0 = 0u;
	req.frame.arg1 = 1u;
	result = zos_ipc_call((unsigned char)handle, &req);
	check("file-write-erofs", result == -ZOS_EROFS);
	check("file-bind-write",
	      zos_ipc_bind("/SYSTEM/services.man", ZFS_O_WRITE, &type) ==
		      -ZOS_EROFS);
	check("file-unbind", zos_ipc_unbind((unsigned char)handle) == 0);
}

/* --- enumeration ---------------------------------------------------------- */

/* Page `path` with one entry per call and print every entry once. */
static void enum_page(const char *path, unsigned int *pages)
{
	unsigned int cookie = 0u;
	unsigned int seen = 0u;
	int result;

	for (;;) {
		result = zos_ns_enum(path, &cookie, entries, 1u);
		check("enum-page-call", result >= 0);
		if (result <= 0) {
			break;
		}
		++seen;
		++*pages;
		zos_printf("ns: %s entry %s type %d%s\r\n", path,
			   entries[0].name, entries[0].type,
			   (entries[0].flags & ZOS_NS_ENTRY_CONTAINER)
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
	result = zos_ns_enum("/", &cookie, entries, NS_ENUM_PAGE);
	check("enum-root", result == 3);
	for (index = 0u; index < (unsigned int)result; ++index) {
		check("enum-root-container",
		      (entries[index].flags & ZOS_NS_ENTRY_CONTAINER) != 0u);
	}
	check("enum-root-cookie", cookie == 3u);
	/* The cookie is the index: a second page is the empty tail. */
	result = zos_ns_enum("/", &cookie, entries, NS_ENUM_PAGE);
	check("enum-root-end", result == 0 && cookie == 3u);

	/* The device tree, one entry per call. */
	enum_page("/dev", &pages);
	check("enum-dev-pages", pages == 4u);

	result = zos_ns_enum("/dev/input", &cookie, entries, NS_ENUM_PAGE);
	check("enum-input", result == 1);
	if (result == 1) {
		check("enum-input-name",
		      strcmp(entries[0].name, "keyboard") == 0);
		check("enum-input-type", entries[0].type == ZOS_TYPE_INPUT);
	}
	cookie = 0u;
	result = zos_ns_enum("/SYSTEM", &cookie, entries, NS_ENUM_PAGE);
	check("enum-system", result == (int)NS_ENUM_PAGE);
	/* The store's root holds the whole recovery set. */
	cookie = 0u;
	for (;;) {
		result = zos_ns_enum("/SYSTEM", &cookie, entries, NS_ENUM_PAGE);
		check("enum-system-page", result >= 0);
		{
			unsigned int k;
			for (k = 0u; k < (unsigned int)result; ++k) {
				zos_printf("ns: /SYSTEM entry %s\r\n",
					   entries[k].name);
			}
		}
		if (result <= 0) {
			break;
		}
		count += (unsigned int)result;
	}
	check("enum-system-count", count == 6u);
	zos_printf("ns: /SYSTEM %u entries\r\n", count);

	check("enum-unknown", zos_ns_enum("/dev/nosuch", &cookie, entries,
					  NS_ENUM_PAGE) == -ZOS_ENOENT);
	check("enum-bad-page", zos_ns_enum("/", &cookie, entries, 0u)
			       == -ZOS_EINVAL);
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
	result = zos_getcwd(cwd, sizeof(cwd));
	check("cwd-initial", result == 1 && cwd[0] == '/' && cwd[1] == '\0');

	check("chdir-system", zos_chdir("/SYSTEM") == 0);
	result = zos_getcwd(cwd, sizeof(cwd));
	check("cwd-system", result == 7 && strcmp(cwd, "/SYSTEM") == 0);
	zos_printf("ns: cwd %s\r\n", cwd);

	/* A relative path resolves against the stored CWD. */
	handle = zos_ipc_bind("services.man", ZFS_O_READ, &type);
	check("cwd-relative-bind", handle >= 3 && type == ZOS_TYPE_FILE);
	if (handle >= 3) {
		check("cwd-relative-unbind",
		      zos_ipc_unbind((unsigned char)handle) == 0);
	}

	/* Provider validation folds case, `..` walks up to the root, and a
	 * refused `chdir` leaves the CWD untouched. */
	check("chdir-case", zos_chdir("/system") == 0);
	check("chdir-parent", zos_chdir("..") == 0);
	result = zos_getcwd(cwd, sizeof(cwd));
	check("cwd-root", result == 1 && cwd[0] == '/' && cwd[1] == '\0');
	check("chdir-dev-input", zos_chdir("/dev/input") == 0);
	result = zos_getcwd(cwd, sizeof(cwd));
	check("cwd-dev-input",
	      result == 10 && strcmp(cwd, "/dev/input") == 0);
	check("chdir-object-refused",
	      zos_chdir("/dev/console") == -ZOS_ENOTDIR);
	check("chdir-absent", zos_chdir("/dev/nosuch") == -ZOS_ENOENT);
	check("chdir-no-provider", zos_chdir("/FS") == -ZOS_ENODEV);
	result = zos_getcwd(cwd, sizeof(cwd));
	check("cwd-survives-refusal",
	      result == 10 && strcmp(cwd, "/dev/input") == 0);

	/* The CWD the inheritance child starts from. */
	check("chdir-dev", zos_chdir("..") == 0);
	result = zos_getcwd(cwd, sizeof(cwd));
	check("cwd-dev", result == 4 && strcmp(cwd, "/dev") == 0);
	zos_printf("ns: cwd %s\r\n", cwd);
}

/* --- the child ------------------------------------------------------------ */

#define NS_CHILD "/SYSTEM/nschild.zex"

static void test_child(void)
{
	char cwd[16];
	int result;

	/* SYS_EXEC resolves the executable through the namespace, gives the
	 * child new standard handles and no other entry, and returns the
	 * child's exit status. */
	result = zos_exec(NS_CHILD, "");
	check("child-status", result == 0);
	zos_printf("ns: exec %s -> %d\r\n", NS_CHILD, result);

	/* The child's `chdir` went to its own CWD slot: the parent's CWD is
	 * exactly the one it set before the exec. */
	result = zos_getcwd(cwd, sizeof(cwd));
	check("cwd-after-child", result == 4 && strcmp(cwd, "/dev") == 0);

	/* The parent's table is its own: the child's binds and unbinds did
	 * not touch it, so a fresh bind still succeeds. */
	check("parent-after-child",
	      zos_ipc_bind("/dev/console", 0u, (unsigned char *)0) >= 3);
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	zos_puts("ns: probe\r\n");
	test_devices();
	test_handle_table();
	test_file();
	test_enum();
	test_cwd();
	test_child();

	zos_printf("ns: %u/%u checks\r\n", checks - failures, checks);
	if (failures == 0u) {
		zos_puts("ns selftest: OK\r\n");
	} else {
		zos_puts("ns selftest: FAIL\r\n");
	}
	return (int)(failures != 0u);
}
