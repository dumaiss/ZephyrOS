/* KonaOS libkos: native file wrappers.
 *
 * `open`/`close` build on bind/unbind; the FILE operations are protocol
 * calls on the returned handle; the name operations (`stat`, `mkdir`,
 * `rmdir`, `unlink`, `rename`) bind the owning provider root and send the
 * operation with the path.  The kernel normalizes and routes every path;
 * this file only applies the documented path-length rule and picks the
 * provider root to bind.
 *
 * `rename` packs both paths into one buffer ("from\0to"), which is the one
 * descriptor a call carries; the buffer is SDK static state and lives in
 * the program's own bank, so the descriptor stays valid.
 */

#include "kos.h"
#include "kos/protocol.h"
#include "kos_internal.h"

/* The two path bytes rename packs: an address inside the program's bank,
 * so the call descriptor can name it. */
static char kos_rename_paths[2u * KOS_PATH_MAX];

/* NUL-terminated length in 1..(KOS_PATH_MAX-1), or 0xFFFF for
 * null/empty/over-long. */
static unsigned int kfs_len(const char *path)
{
	unsigned int n = 0;

	if (path == (const char *)0) {
		return 0xFFFFu;
	}
	while (path[n] != '\0') {
		if (n >= (unsigned int)(KOS_PATH_MAX - 1u)) {
			return 0xFFFFu;
		}
		++n;
	}
	if (n == 0u) {
		return 0xFFFFu;
	}
	return n;
}

static int kfs_call(unsigned char handle, unsigned char op, void *buffer,
		    unsigned int length, unsigned char direction)
{
	struct kos_ipc_request req;

	req.frame.op = op;
	req.frame.flags = 0u;
	req.frame.desc = KOS_IPC_DESC_NONE;
	req.frame.arg0 = 0u;
	req.frame.arg1 = (unsigned int)length;
	req.buffer = buffer;
	req.length = length;
	req.direction = direction;
	return call(handle, &req);
}

/* The provider root of `path`: its first component for an absolute path,
 * the CWD's first component otherwise (a relative operation belongs to
 * whichever provider owns the CWD).  Returns the root length (1..127,
 * including the leading slash) or 0xFFFF. */
static unsigned int kfs_root_of(const char *path, char *root,
				      unsigned int root_size)
{
	unsigned int n;
	unsigned int i;

	if (path[0] == '/') {
		n = 1u;
		while (path[n] != '\0' && path[n] != '/') {
			++n;
		}
		if (n + 1u > root_size) {
			return 0xFFFFu;
		}
		for (i = 0u; i < n; ++i) {
			root[i] = path[i];
		}
		root[n] = '\0';
		return n;
	}
	/* A relative operation belongs to whichever provider owns the CWD. */
	if (getcwd(root, root_size) < 0) {
		return 0xFFFFu;
	}
	n = 1u;
	while (root[n] != '\0' && root[n] != '/') {
		++n;
	}
	root[n] = '\0';
	return n;
}

/* Bind the owning provider root for a name operation and return its
 * filesystem-namespace handle, or the negated error. */
static int kfs_bind_root(const char *path)
{
	char root[KOS_PATH_MAX];
	unsigned int length = kfs_root_of(path, root, sizeof(root));
	unsigned char type = 0u;
	int handle;

	if (length == 0xFFFFu) {
		return -KOS_EINVAL;
	}
	handle = bind(root, 0u, &type);
	if (handle < 0) {
		return handle;
	}
	if (type != KOS_TYPE_FS) {
		(void)unbind((unsigned char)handle);
		return -KOS_EINVAL;
	}
	return handle;
}

/* One name operation: bind the provider, send the path, unbind. */
static int kfs_path_op(const char *path, unsigned char op)
{
	unsigned int length = kfs_len(path);
	int handle;
	int result;

	if (length == 0xFFFFu) {
		return -KOS_EINVAL;
	}
	handle = kfs_bind_root(path);
	if (handle < 0) {
		return handle;
	}
	result = kfs_call((unsigned char)handle, op, (void *)path, length,
			  KOS_IPC_DESC_IN);
	(void)unbind((unsigned char)handle);
	return result;
}

int open(const char *path, unsigned char flags)
{
	unsigned char type = 0u;
	int handle = bind(path, (unsigned int)flags, &type);

	if (handle < 0) {
		return handle;
	}
	if (type != KOS_TYPE_FILE) {
		(void)unbind((unsigned char)handle);
		return -KOS_EINVAL;
	}
	return handle;
}

int close(unsigned int handle)
{
	if (handle < KOS_HANDLE_FIRST_BOUND) {
		return -KOS_EINVAL;
	}
	return unbind((unsigned char)handle);
}

int read(unsigned int handle, void *buffer, unsigned int length)
{
	if (buffer == (void *)0 || length == 0u) {
		return -KOS_EINVAL;
	}
	return kfs_call((unsigned char)handle, KFILE_OP_READ, buffer, length,
			KOS_IPC_DESC_OUT);
}

int write(unsigned int handle, const void *buffer, unsigned int length)
{
	if (buffer == (const void *)0 || length == 0u) {
		return -KOS_EINVAL;
	}
	return kfs_call((unsigned char)handle, KFILE_OP_WRITE, (void *)buffer,
			length, KOS_IPC_DESC_IN);
}

long seek(unsigned int handle, long offset, unsigned char whence)
{
	struct kos_ipc_request req;
	int result;

	req.frame.op = KFILE_OP_SEEK;
	req.frame.flags = 0u;
	req.frame.desc = KOS_IPC_DESC_NONE;
	req.frame.arg0 = whence;
	req.frame.arg1 = 0u;
	req.frame.payload[1] = (unsigned char)((unsigned long)offset & 0xFFu);
	req.frame.payload[2] = (unsigned char)(((unsigned long)offset >> 8) & 0xFFu);
	req.frame.payload[3] = (unsigned char)(((unsigned long)offset >> 16) & 0xFFu);
	req.frame.payload[4] = (unsigned char)(((unsigned long)offset >> 24) & 0xFFu);
	req.buffer = (void *)0;
	req.length = 0u;
	req.direction = KOS_IPC_DESC_IN;
	result = call((unsigned char)handle, &req);
	if (result < 0) {
		return (long)result;
	}
	return (long)((unsigned long)req.frame.payload[1] |
		      ((unsigned long)req.frame.payload[2] << 8) |
		      ((unsigned long)req.frame.payload[3] << 16) |
		      ((unsigned long)req.frame.payload[4] << 24));
}

int sync(unsigned int handle)
{
	struct kos_ipc_request req;

	req.frame.op = KFILE_OP_SYNC;
	req.frame.flags = 0u;
	req.frame.desc = KOS_IPC_DESC_NONE;
	req.frame.arg0 = 0u;
	req.frame.arg1 = 0u;
	req.buffer = (void *)0;
	req.length = 0u;
	req.direction = KOS_IPC_DESC_IN;
	return call((unsigned char)handle, &req);
}

int stat(const char *path, struct kfs_stat *out)
{
	unsigned int length = kfs_len(path);
	struct kos_ipc_request req;
	unsigned char *src;
	unsigned char *dst = (unsigned char *)out;
	unsigned int i;
	int handle;
	int result;

	if (out == (struct kfs_stat *)0) {
		return -KOS_EINVAL;
	}
	if (length == 0xFFFFu) {
		return -KOS_EINVAL;
	}
	handle = kfs_bind_root(path);
	if (handle < 0) {
		return handle;
	}
	req.frame.op = KFS_OP_STAT;
	req.frame.flags = 0u;
	req.frame.desc = KOS_IPC_DESC_NONE;
	req.frame.arg0 = 0u;
	req.frame.arg1 = 0u;
	req.buffer = (void *)path;
	req.length = length;
	req.direction = KOS_IPC_DESC_IN;
	result = call((unsigned char)handle, &req);
	(void)unbind((unsigned char)handle);
	if (result < 0) {
		return result;
	}
	src = (unsigned char *)&req.frame.payload[0];
	for (i = 0u; i < (unsigned int)sizeof(struct kfs_stat); ++i) {
		dst[i] = src[i];
	}
	return 0;
}

int mkdir(const char *path)
{
	return kfs_path_op(path, KFS_OP_MKDIR);
}

int rmdir(const char *path)
{
	return kfs_path_op(path, KFS_OP_RMDIR);
}

int unlink(const char *path)
{
	return kfs_path_op(path, KFS_OP_UNLINK);
}

int rename(const char *oldpath, const char *newpath)
{
	unsigned int from_len = kfs_len(oldpath);
	unsigned int to_len = kfs_len(newpath);
	unsigned int i;
	int handle;
	int result;

	if (from_len == 0xFFFFu || to_len == 0xFFFFu) {
		return -KOS_EINVAL;
	}
	for (i = 0u; i < from_len; ++i) {
		kos_rename_paths[i] = oldpath[i];
	}
	for (i = 0u; i < to_len; ++i) {
		kos_rename_paths[from_len + 1u + i] = newpath[i];
	}
	kos_rename_paths[from_len] = '\0';
	handle = kfs_bind_root(oldpath);
	if (handle < 0) {
		return handle;
	}
	result = kfs_call((unsigned char)handle, KFS_OP_RENAME, kos_rename_paths,
			  (unsigned int)(from_len + 1u + to_len),
			  KOS_IPC_DESC_IN);
	(void)unbind((unsigned char)handle);
	return result;
}
