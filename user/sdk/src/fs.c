/* ZephyrOS libzos: native file wrappers.
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

#include "zos.h"
#include "zos/protocol.h"
#include "zos_internal.h"

/* The two path bytes rename packs: an address inside the program's bank,
 * so the call descriptor can name it. */
static char zos_rename_paths[2u * ZOS_PATH_MAX];

/* NUL-terminated length in 1..(ZOS_PATH_MAX-1), or 0xFFFF for
 * null/empty/over-long. */
static unsigned int zfs_len(const char *path)
{
	unsigned int n = 0;

	if (path == (const char *)0) {
		return 0xFFFFu;
	}
	while (path[n] != '\0') {
		if (n >= (unsigned int)(ZOS_PATH_MAX - 1u)) {
			return 0xFFFFu;
		}
		++n;
	}
	if (n == 0u) {
		return 0xFFFFu;
	}
	return n;
}

static int zfs_call(unsigned char handle, unsigned char op, void *buffer,
		    unsigned int length, unsigned char direction)
{
	struct zos_ipc_request req;

	req.frame.op = op;
	req.frame.flags = 0u;
	req.frame.desc = ZOS_IPC_DESC_NONE;
	req.frame.arg0 = 0u;
	req.frame.arg1 = (unsigned int)length;
	req.buffer = buffer;
	req.length = length;
	req.direction = direction;
	return zos_ipc_call(handle, &req);
}

/* The provider root of `path`: its first component for an absolute path,
 * the CWD's first component otherwise (a relative operation belongs to
 * whichever provider owns the CWD).  Returns the root length (1..127,
 * including the leading slash) or 0xFFFF. */
static unsigned int zfs_provider_root(const char *path, char *root,
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
	if (zos_getcwd(root, root_size) < 0) {
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
static int zfs_provider(const char *path)
{
	char root[ZOS_PATH_MAX];
	unsigned int length = zfs_provider_root(path, root, sizeof(root));
	unsigned char type = 0u;
	int handle;

	if (length == 0xFFFFu) {
		return -ZOS_EINVAL;
	}
	handle = zos_ipc_bind(root, 0u, &type);
	if (handle < 0) {
		return handle;
	}
	if (type != ZOS_TYPE_FS) {
		(void)zos_ipc_unbind((unsigned char)handle);
		return -ZOS_EINVAL;
	}
	return handle;
}

/* One name operation: bind the provider, send the path, unbind. */
static int zfs_path_op(const char *path, unsigned char op)
{
	unsigned int length = zfs_len(path);
	int handle;
	int result;

	if (length == 0xFFFFu) {
		return -ZOS_EINVAL;
	}
	handle = zfs_provider(path);
	if (handle < 0) {
		return handle;
	}
	result = zfs_call((unsigned char)handle, op, (void *)path, length,
			  ZOS_IPC_DESC_IN);
	(void)zos_ipc_unbind((unsigned char)handle);
	return result;
}

int zos_open(const char *path, unsigned char flags)
{
	unsigned char type = 0u;
	int handle = zos_ipc_bind(path, (unsigned int)flags, &type);

	if (handle < 0) {
		return handle;
	}
	if (type != ZOS_TYPE_FILE) {
		(void)zos_ipc_unbind((unsigned char)handle);
		return -ZOS_EINVAL;
	}
	return handle;
}

int zos_close(unsigned int handle)
{
	if (handle < ZOS_HANDLE_FIRST_BOUND) {
		return -ZOS_EINVAL;
	}
	return zos_ipc_unbind((unsigned char)handle);
}

int zos_read(unsigned int handle, void *buffer, unsigned int length)
{
	if (buffer == (void *)0 || length == 0u) {
		return -ZOS_EINVAL;
	}
	return zfs_call((unsigned char)handle, ZFILE_OP_READ, buffer, length,
			ZOS_IPC_DESC_OUT);
}

int zos_write(unsigned int handle, const void *buffer, unsigned int length)
{
	if (buffer == (const void *)0 || length == 0u) {
		return -ZOS_EINVAL;
	}
	return zfs_call((unsigned char)handle, ZFILE_OP_WRITE, (void *)buffer,
			length, ZOS_IPC_DESC_IN);
}

long zos_seek(unsigned int handle, long offset, unsigned char whence)
{
	struct zos_ipc_request req;
	int result;

	req.frame.op = ZFILE_OP_SEEK;
	req.frame.flags = 0u;
	req.frame.desc = ZOS_IPC_DESC_NONE;
	req.frame.arg0 = whence;
	req.frame.arg1 = 0u;
	req.frame.payload[1] = (unsigned char)((unsigned long)offset & 0xFFu);
	req.frame.payload[2] = (unsigned char)(((unsigned long)offset >> 8) & 0xFFu);
	req.frame.payload[3] = (unsigned char)(((unsigned long)offset >> 16) & 0xFFu);
	req.frame.payload[4] = (unsigned char)(((unsigned long)offset >> 24) & 0xFFu);
	req.buffer = (void *)0;
	req.length = 0u;
	req.direction = ZOS_IPC_DESC_IN;
	result = zos_ipc_call((unsigned char)handle, &req);
	if (result < 0) {
		return (long)result;
	}
	return (long)((unsigned long)req.frame.payload[1] |
		      ((unsigned long)req.frame.payload[2] << 8) |
		      ((unsigned long)req.frame.payload[3] << 16) |
		      ((unsigned long)req.frame.payload[4] << 24));
}

int zos_sync(unsigned int handle)
{
	struct zos_ipc_request req;

	req.frame.op = ZFILE_OP_SYNC;
	req.frame.flags = 0u;
	req.frame.desc = ZOS_IPC_DESC_NONE;
	req.frame.arg0 = 0u;
	req.frame.arg1 = 0u;
	req.buffer = (void *)0;
	req.length = 0u;
	req.direction = ZOS_IPC_DESC_IN;
	return zos_ipc_call((unsigned char)handle, &req);
}

int zos_stat(const char *path, struct zfs_stat *out)
{
	unsigned int length = zfs_len(path);
	struct zos_ipc_request req;
	unsigned char *src;
	unsigned char *dst = (unsigned char *)out;
	unsigned int i;
	int handle;
	int result;

	if (out == (struct zfs_stat *)0) {
		return -ZOS_EINVAL;
	}
	if (length == 0xFFFFu) {
		return -ZOS_EINVAL;
	}
	handle = zfs_provider(path);
	if (handle < 0) {
		return handle;
	}
	req.frame.op = ZFS_OP_STAT;
	req.frame.flags = 0u;
	req.frame.desc = ZOS_IPC_DESC_NONE;
	req.frame.arg0 = 0u;
	req.frame.arg1 = 0u;
	req.buffer = (void *)path;
	req.length = length;
	req.direction = ZOS_IPC_DESC_IN;
	result = zos_ipc_call((unsigned char)handle, &req);
	(void)zos_ipc_unbind((unsigned char)handle);
	if (result < 0) {
		return result;
	}
	src = (unsigned char *)&req.frame.payload[0];
	for (i = 0u; i < (unsigned int)sizeof(struct zfs_stat); ++i) {
		dst[i] = src[i];
	}
	return 0;
}

int zos_mkdir(const char *path)
{
	return zfs_path_op(path, ZFS_OP_MKDIR);
}

int zos_rmdir(const char *path)
{
	return zfs_path_op(path, ZFS_OP_RMDIR);
}

int zos_unlink(const char *path)
{
	return zfs_path_op(path, ZFS_OP_UNLINK);
}

int zos_rename(const char *oldpath, const char *newpath)
{
	unsigned int from_len = zfs_len(oldpath);
	unsigned int to_len = zfs_len(newpath);
	unsigned int i;
	int handle;
	int result;

	if (from_len == 0xFFFFu || to_len == 0xFFFFu) {
		return -ZOS_EINVAL;
	}
	for (i = 0u; i < from_len; ++i) {
		zos_rename_paths[i] = oldpath[i];
	}
	for (i = 0u; i < to_len; ++i) {
		zos_rename_paths[from_len + 1u + i] = newpath[i];
	}
	zos_rename_paths[from_len] = '\0';
	handle = zfs_provider(oldpath);
	if (handle < 0) {
		return handle;
	}
	result = zfs_call((unsigned char)handle, ZFS_OP_RENAME, zos_rename_paths,
			  (unsigned int)(from_len + 1u + to_len),
			  ZOS_IPC_DESC_IN);
	(void)zos_ipc_unbind((unsigned char)handle);
	return result;
}
