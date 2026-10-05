/* ZephyrOS M9 test-only writable RAM filesystem provider (driver ABI v3).
 *
 * This module is packed as `ramfs.zmod` and loaded only by the M9 provider
 * acceptance image; it never enters a production manifest or the production
 * ROMFS.  Its init registers the path-based `ramfs` provider through the
 * driver ABI v3 `fs_register` callback, exercising module provider
 * registration and the loader's rollback transaction.
 *
 * Bounded storage: 16 nodes and 2048 bytes of file data per instance, two
 * instances total.  Node paths are provider-relative and matched with the
 * ASCII case-insensitive rule the provider ABI requires (architecture
 * section 32.8).
 *
 * Documented reserved control names (case-insensitive, at any directory):
 *   /FAILWRITE  creating it makes the next write fail with ZOS_EIO and
 *               commit nothing; the control is consumed by that write
 *   /STALE      creating it advances the instance generation, so every
 *               open handle reports ZOS_ESTALE until it is removed
 *   /STATS      virtual read-only file exposing the per-operation call
 *               counters and the open-handle count (struct ramfs_stats)
 * The mount source string "refuse" makes mount() fail with ZOS_EIO.  The
 * two instance sources are "ramfs.0" and "ramfs.1".
 */

#include "zephyros/driver.h"
#include "zephyros/errno.h"
#include "zephyros/fs.h"
#include "zephyros/vfs.h"

#define RAMFS_INSTANCES 2u
#define RAMFS_NODES 16u
#define RAMFS_HANDLES 4u
#define RAMFS_DATA_SIZE 2048u
/* The acceptance paths are short; a longer provider path is EINVAL. */
#define RAMFS_PATH_MAX 24u

#define RAMFS_ROOT_NODE 0xFFFFu

#define RAMFS_H_WRITABLE 0x01u
#define RAMFS_H_STATS    0x02u
#define RAMFS_H_DIR      0x04u

#define RAMFS_C_MOUNT    0u
#define RAMFS_C_OPEN     1u
#define RAMFS_C_CLOSE    2u
#define RAMFS_C_READ     3u
#define RAMFS_C_WRITE    4u
#define RAMFS_C_SYNC     5u
#define RAMFS_C_STAT     6u
#define RAMFS_C_OPENDIR  7u
#define RAMFS_C_READDIR  8u
#define RAMFS_C_CLOSEDIR 9u
#define RAMFS_C_MKDIR    10u
#define RAMFS_C_RMDIR    11u
#define RAMFS_C_UNLINK   12u
#define RAMFS_C_RENAME   13u
#define RAMFS_C_COUNT    14u

struct ramfs_stats {
	uint16_t open_handles;
	uint16_t calls[RAMFS_C_COUNT];
};

struct ramfs_node {
	uint8_t used;
	uint8_t type;
	uint16_t offset; /* start of the file's data region */
	uint32_t size;
	char path[RAMFS_PATH_MAX];
};

struct ramfs_handle {
	uint8_t used;
	uint8_t flags;
	uint16_t node;       /* node index, or RAMFS_ROOT_NODE */
	uint16_t generation; /* stale when it differs from the instance */
	uint16_t cursor;     /* readdir position over the node table */
};

struct ramfs_instance {
	/* `stats` is first so the /STATS virtual file can expose its exact
	 * little-endian image straight from the instance. */
	struct ramfs_stats stats;
	uint8_t mounted;
	uint8_t stale;
	uint8_t fail_next_write;
	uint16_t generation;
	uint16_t data_used;
	struct ramfs_node nodes[RAMFS_NODES];
	struct ramfs_handle handles[RAMFS_HANDLES];
	uint8_t data[RAMFS_DATA_SIZE];
};

static struct ramfs_instance ramfs_instances[RAMFS_INSTANCES];

/* Provider scratch: provider paths are shorter than RAMFS_PATH_MAX and a
 * provider operation is never re-entered, so static buffers are enough; no
 * gate-stack-sized automatic object is declared. */
static char ramfs_scratch[RAMFS_PATH_MAX];
static char ramfs_name[RAMFS_PATH_MAX];
static char ramfs_rebuilt[RAMFS_PATH_MAX];

/* --- small helpers ---------------------------------------------------- */

static uint16_t ramfs_length(const char *text)
{
	uint16_t n = 0;

	while (text[n] != '\0') {
		++n;
	}
	return n;
}

static uint8_t ramfs_fold(uint8_t c)
{
	if (c >= 'a' && c <= 'z') {
		return (uint8_t)(c - 32u);
	}
	return c;
}

static uint8_t ramfs_equal(const char *a, const char *b)
{
	for (;;) {
		uint8_t ca = ramfs_fold((uint8_t)*a++);
		uint8_t cb = ramfs_fold((uint8_t)*b++);

		if (ca != cb) {
			return 0;
		}
		if (ca == '\0') {
			return 1;
		}
	}
}

/* True when `path` is `prefix` itself or lies below it. */
static uint8_t ramfs_prefix(const char *path, const char *prefix)
{
	uint16_t i = 0;

	for (;;) {
		uint8_t cp = ramfs_fold((uint8_t)prefix[i]);

		if (cp == '\0') {
			return (uint8_t)(path[i] == '\0' || path[i] == '/');
		}
		if (ramfs_fold((uint8_t)path[i]) != cp) {
			return 0;
		}
		++i;
	}
}

static uint8_t ramfs_too_long(const char *path)
{
	return (uint8_t)(ramfs_length(path) >= RAMFS_PATH_MAX);
}

static void ramfs_copy(char *dst, const char *src)
{
	uint16_t i = 0;

	while (src[i] != '\0' && i + 1u < RAMFS_PATH_MAX) {
		dst[i] = src[i];
		++i;
	}
	dst[i] = '\0';
}

/* Copy a component into a dirent name and return its length including the
 * terminating NUL.  `src` arrives as an argument so its (global) address is
 * a word relocation, not a byte-fragmented store. */
static uint16_t ramfs_copy_name(char *dst, const char *src)
{
	uint16_t n = 0;

	while (*src != '\0' && n + 1u < ZFS_NAME_MAX) {
		dst[n++] = *src++;
	}
	dst[n] = '\0';
	return (uint16_t)(n + 1u);
}

/* Build newpath + (tail) into `dst`, bounded by RAMFS_PATH_MAX. */
static void ramfs_make_path(char *dst, const char *newpath, const char *tail)
{
	uint16_t k = 0;

	while (newpath[k] != '\0' && k + 1u < RAMFS_PATH_MAX) {
		dst[k] = newpath[k];
		++k;
	}
	while (*tail != '\0' && k + 1u < RAMFS_PATH_MAX) {
		dst[k++] = *tail++;
	}
	dst[k] = '\0';
}

/* Split `path` into its parent directory and last component. */
static void ramfs_split(const char *path, char *parent, char *name)
{
	uint16_t i;
	uint16_t last = 0;
	uint16_t n = 0;

	for (i = 0; path[i] != '\0'; ++i) {
		if (path[i] == '/') {
			last = i;
		}
	}
	if (last == 0) {
		parent[0] = '/';
		parent[1] = '\0';
	} else {
		for (i = 0; i < last; ++i) {
			parent[i] = path[i];
		}
		parent[last] = '\0';
	}
	i = (uint16_t)(last + 1u);
	while (path[i] != '\0' && n + 1u < RAMFS_PATH_MAX) {
		name[n++] = path[i++];
	}
	name[n] = '\0';
}

static struct ramfs_node *ramfs_find(struct ramfs_instance *in,
				     const char *path)
{
	uint16_t i;

	for (i = 0; i < RAMFS_NODES; ++i) {
		if (in->nodes[i].used &&
		    ramfs_equal(in->nodes[i].path, path)) {
			return &in->nodes[i];
		}
	}
	return (struct ramfs_node *)0;
}

/* Node index by identity.  Scanning avoids a pointer difference, which
 * would pull a signed-division helper into the module. */
static uint16_t ramfs_node_index(struct ramfs_instance *in,
				 const struct ramfs_node *node)
{
	uint16_t i;

	for (i = 0; i < RAMFS_NODES; ++i) {
		if (&in->nodes[i] == node) {
			return i;
		}
	}
	return RAMFS_ROOT_NODE;
}

static uint8_t ramfs_parent_is_dir(struct ramfs_instance *in,
				   const char *path)
{
	struct ramfs_node *parent;

	ramfs_split(path, ramfs_scratch, ramfs_name);
	if (ramfs_scratch[0] == '/' && ramfs_scratch[1] == '\0') {
		return 1;
	}
	parent = ramfs_find(in, ramfs_scratch);
	return (uint8_t)(parent != (struct ramfs_node *)0 &&
			 parent->type == ZFS_TYPE_DIR);
}

static uint8_t ramfs_has_children(struct ramfs_instance *in,
				  const char *dir)
{
	uint16_t i;

	for (i = 0; i < RAMFS_NODES; ++i) {
		if (!in->nodes[i].used) {
			continue;
		}
		ramfs_split(in->nodes[i].path, ramfs_scratch, ramfs_name);
		if (ramfs_equal(ramfs_scratch, dir)) {
			return 1;
		}
	}
	return 0;
}

static struct ramfs_node *ramfs_alloc_node(struct ramfs_instance *in)
{
	uint16_t i;

	for (i = 0; i < RAMFS_NODES; ++i) {
		if (!in->nodes[i].used) {
			return &in->nodes[i];
		}
	}
	return (struct ramfs_node *)0;
}

static struct ramfs_handle *ramfs_alloc_handle(struct ramfs_instance *in)
{
	uint16_t i;

	for (i = 0; i < RAMFS_HANDLES; ++i) {
		if (!in->handles[i].used) {
			return &in->handles[i];
		}
	}
	return (struct ramfs_handle *)0;
}

static void ramfs_zero_instance(struct ramfs_instance *in)
{
	uint8_t *p = (uint8_t *)in;
	uint16_t i;

	for (i = 0; i < (uint16_t)sizeof(struct ramfs_instance); ++i) {
		p[i] = 0;
	}
}

static void ramfs_node_reset(struct ramfs_node *node)
{
	node->used = 0;
	node->type = 0;
	node->offset = 0;
	node->size = 0;
	node->path[0] = '\0';
}

/* Reserved control names.  `STALE` bumps the generation; `FAILWRITE` arms a
 * one-shot failed write.  Both may be created as files or directories. */
static void ramfs_apply_control(struct ramfs_instance *in, const char *name)
{
	if (ramfs_equal(name, "STALE")) {
		in->stale = 1;
		++in->generation;
	} else if (ramfs_equal(name, "FAILWRITE")) {
		in->fail_next_write = 1;
	}
}

static void ramfs_fill_stat(struct zfs_stat *out, uint8_t type, uint32_t size)
{
	out->type = type;
	out->attributes = 0;
	out->reserved = 0;
	out->size = size;
	out->ctime = ZFS_TIME_UNKNOWN;
	out->mtime = ZFS_TIME_UNKNOWN;
}

/* --- provider operations ---------------------------------------------- */

static int ramfs_mount(void *provider_state, const char *source, void **fs)
{
	struct ramfs_instance *in;
	uint8_t index = 0;
	uint16_t i;

	(void)provider_state;
	if (ramfs_equal(source, "refuse")) {
		return -ZOS_EIO;
	}
	for (i = 0; source[i] != '\0'; ++i) {
		if (source[i] == '.' && source[i + 1u] >= '0' &&
		    source[i + 1u] <= '9') {
			index = (uint8_t)(source[i + 1u] - '0');
			break;
		}
	}
	if (index >= RAMFS_INSTANCES) {
		return -ZOS_EINVAL;
	}
	in = &ramfs_instances[index];
	if (in->mounted) {
		return -ZOS_EBUSY;
	}
	ramfs_zero_instance(in);
	in->mounted = 1;
	in->stats.calls[RAMFS_C_MOUNT] = 1u;
	*fs = in;
	return 0;
}

static int ramfs_open(void *fs, const char *path, uint16_t flags,
		      void **file, uint32_t *size)
{
	struct ramfs_instance *in = (struct ramfs_instance *)fs;
	struct ramfs_node *node;
	struct ramfs_handle *handle;

	++in->stats.calls[RAMFS_C_OPEN];
	if (ramfs_too_long(path)) {
		return -ZOS_EINVAL;
	}
	if (ramfs_equal(path, "/STATS")) {
		handle = ramfs_alloc_handle(in);
		if (handle == (struct ramfs_handle *)0) {
			return -ZOS_EMFILE;
		}
		handle->used = 1;
		handle->flags = RAMFS_H_STATS;
		handle->node = RAMFS_ROOT_NODE;
		handle->generation = in->generation;
		handle->cursor = 0;
		++in->stats.open_handles;
		*file = handle;
		*size = (uint32_t)sizeof(struct ramfs_stats);
		return 0;
	}
	node = ramfs_find(in, path);
	if (node == (struct ramfs_node *)0) {
		if ((flags & ZFS_O_CREATE) == 0) {
			return -ZOS_ENOENT;
		}
		if (!ramfs_parent_is_dir(in, path)) {
			return -ZOS_ENOENT;
		}
		node = ramfs_alloc_node(in);
		if (node == (struct ramfs_node *)0) {
			return -ZOS_ENOSPC;
		}
		node->used = 1;
		node->type = ZFS_TYPE_FILE;
		node->offset = in->data_used;
		node->size = 0;
		ramfs_copy(node->path, path);
		ramfs_split(path, ramfs_scratch, ramfs_name);
		ramfs_apply_control(in, ramfs_name);
	} else {
		if ((flags & (ZFS_O_CREATE | ZFS_O_EXCL)) ==
		    (ZFS_O_CREATE | ZFS_O_EXCL)) {
			return -ZOS_EEXIST;
		}
		if (node->type == ZFS_TYPE_DIR) {
			return -ZOS_EISDIR;
		}
		if ((flags & ZFS_O_TRUNC) != 0) {
			node->size = 0;
		}
	}
	handle = ramfs_alloc_handle(in);
	if (handle == (struct ramfs_handle *)0) {
		return -ZOS_EMFILE;
	}
	handle->used = 1;
	handle->flags = (uint8_t)((flags & ZFS_O_WRITE) != 0
				  ? RAMFS_H_WRITABLE : 0u);
	handle->node = ramfs_node_index(in, node);
	handle->generation = in->generation;
	handle->cursor = 0;
	++in->stats.open_handles;
	*file = handle;
	*size = node->size;
	return 0;
}

static int ramfs_close(void *fs, void *file)
{
	struct ramfs_instance *in = (struct ramfs_instance *)fs;
	struct ramfs_handle *handle = (struct ramfs_handle *)file;

	++in->stats.calls[RAMFS_C_CLOSE];
	if (handle == (struct ramfs_handle *)0 || handle->used == 0) {
		return -ZOS_EINVAL;
	}
	handle->used = 0;
	if (in->stats.open_handles > 0) {
		--in->stats.open_handles;
	}
	return 0;
}

static int ramfs_read(void *fs, void *file, uint32_t offset, void *buf,
		      uint16_t len)
{
	struct ramfs_instance *in = (struct ramfs_instance *)fs;
	struct ramfs_handle *handle = (struct ramfs_handle *)file;
	struct ramfs_node *node;
	uint8_t *dst = (uint8_t *)buf;
	uint16_t i;

	++in->stats.calls[RAMFS_C_READ];
	if (handle == (struct ramfs_handle *)0 || handle->used == 0) {
		return -ZOS_EINVAL;
	}
	if (handle->generation != in->generation) {
		return -ZOS_ESTALE;
	}
	if ((handle->flags & RAMFS_H_STATS) != 0) {
		/* `in` is a local pointer, so the byte image of the counters
		 * is reached without a global-address byte fragment. */
		const uint8_t *base = (const uint8_t *)in;
		uint16_t total = (uint16_t)sizeof(struct ramfs_stats);
		uint16_t at = (uint16_t)offset;

		if (at >= total) {
			return 0;
		}
		if ((uint32_t)len > (uint32_t)(total - at)) {
			len = (uint16_t)(total - at);
		}
		for (i = 0; i < len; ++i) {
			dst[i] = base[at + i];
		}
		return (int)len;
	}
	node = &in->nodes[handle->node];
	if (offset >= node->size) {
		return 0;
	}
	if ((uint32_t)len > (node->size - offset)) {
		len = (uint16_t)(node->size - offset);
	}
	for (i = 0; i < len; ++i) {
		dst[i] = in->data[node->offset + offset + i];
	}
	return (int)len;
}

static int ramfs_write(void *fs, void *file, uint32_t offset,
		       const void *buf, uint16_t len)
{
	struct ramfs_instance *in = (struct ramfs_instance *)fs;
	struct ramfs_handle *handle = (struct ramfs_handle *)file;
	struct ramfs_node *node;
	const uint8_t *src = (const uint8_t *)buf;
	uint16_t i;

	++in->stats.calls[RAMFS_C_WRITE];
	if (handle == (struct ramfs_handle *)0 || handle->used == 0) {
		return -ZOS_EINVAL;
	}
	if (handle->generation != in->generation) {
		return -ZOS_ESTALE;
	}
	if (in->fail_next_write != 0) {
		in->fail_next_write = 0;
		return -ZOS_EIO; /* commit nothing */
	}
	if ((handle->flags & RAMFS_H_WRITABLE) == 0) {
		return -ZOS_EROFS;
	}
	node = &in->nodes[handle->node];
	if ((uint32_t)(node->offset + offset + len) > RAMFS_DATA_SIZE) {
		return -ZOS_ENOSPC;
	}
	for (i = 0; i < len; ++i) {
		in->data[node->offset + offset + i] = src[i];
	}
	if ((uint32_t)(offset + len) > node->size) {
		node->size = offset + len;
	}
	if ((uint16_t)(node->offset + offset + len) > in->data_used) {
		in->data_used = (uint16_t)(node->offset + offset + len);
	}
	return 0;
}

static int ramfs_sync(void *fs, void *file)
{
	struct ramfs_instance *in = (struct ramfs_instance *)fs;
	struct ramfs_handle *handle = (struct ramfs_handle *)file;

	++in->stats.calls[RAMFS_C_SYNC];
	if (handle == (struct ramfs_handle *)0 || handle->used == 0) {
		return -ZOS_EINVAL;
	}
	if (handle->generation != in->generation) {
		return -ZOS_ESTALE;
	}
	return 0;
}

static int ramfs_stat(void *fs, const char *path, struct zfs_stat *out)
{
	struct ramfs_instance *in = (struct ramfs_instance *)fs;
	struct ramfs_node *node;

	++in->stats.calls[RAMFS_C_STAT];
	if (ramfs_too_long(path)) {
		return -ZOS_EINVAL;
	}
	if (ramfs_equal(path, "/")) {
		ramfs_fill_stat(out, ZFS_TYPE_DIR, 0);
		return 0;
	}
	if (ramfs_equal(path, "/STATS")) {
		ramfs_fill_stat(out, ZFS_TYPE_FILE,
				(uint32_t)sizeof(struct ramfs_stats));
		return 0;
	}
	node = ramfs_find(in, path);
	if (node == (struct ramfs_node *)0) {
		return -ZOS_ENOENT;
	}
	ramfs_fill_stat(out, node->type, node->size);
	return 0;
}

static int ramfs_opendir(void *fs, const char *path, void **dir)
{
	struct ramfs_instance *in = (struct ramfs_instance *)fs;
	struct ramfs_node *node;
	struct ramfs_handle *handle;

	++in->stats.calls[RAMFS_C_OPENDIR];
	if (ramfs_too_long(path)) {
		return -ZOS_EINVAL;
	}
	if (ramfs_equal(path, "/")) {
		node = (struct ramfs_node *)0;
	} else {
		node = ramfs_find(in, path);
		if (node == (struct ramfs_node *)0) {
			return -ZOS_ENOENT;
		}
		if (node->type != ZFS_TYPE_DIR) {
			return -ZOS_ENOTDIR;
		}
	}
	handle = ramfs_alloc_handle(in);
	if (handle == (struct ramfs_handle *)0) {
		return -ZOS_EMFILE;
	}
	handle->used = 1;
	handle->flags = RAMFS_H_DIR;
	handle->node = (node == (struct ramfs_node *)0)
			       ? RAMFS_ROOT_NODE
			       : ramfs_node_index(in, node);
	handle->generation = in->generation;
	handle->cursor = 0;
	++in->stats.open_handles;
	*dir = handle;
	return 0;
}

static int ramfs_readdir(void *fs, void *dir, struct zfs_dirent *out)
{
	struct ramfs_instance *in = (struct ramfs_instance *)fs;
	struct ramfs_handle *handle = (struct ramfs_handle *)dir;
	const char *dirpath;
	uint16_t i;

	++in->stats.calls[RAMFS_C_READDIR];
	if (handle == (struct ramfs_handle *)0 || handle->used == 0) {
		return -ZOS_EINVAL;
	}
	if (handle->generation != in->generation) {
		return -ZOS_ESTALE;
	}
	dirpath = (handle->node == RAMFS_ROOT_NODE)
			  ? "/"
			  : in->nodes[handle->node].path;
	for (i = handle->cursor; i < RAMFS_NODES; ++i) {
		if (!in->nodes[i].used) {
			continue;
		}
		ramfs_split(in->nodes[i].path, ramfs_scratch, ramfs_name);
		if (!ramfs_equal(ramfs_scratch, dirpath)) {
			continue;
		}
		out->type = in->nodes[i].type;
		out->attributes = 0;
		out->size = in->nodes[i].size;
		out->ctime = ZFS_TIME_UNKNOWN;
		out->mtime = ZFS_TIME_UNKNOWN;
		out->name_length = ramfs_copy_name(out->name, ramfs_name);
		handle->cursor = (uint16_t)(i + 1u);
		return 1;
	}
	return 0;
}

static int ramfs_closedir(void *fs, void *dir)
{
	return ramfs_close(fs, dir);
}

static int ramfs_mkdir(void *fs, const char *path)
{
	struct ramfs_instance *in = (struct ramfs_instance *)fs;
	struct ramfs_node *node;

	++in->stats.calls[RAMFS_C_MKDIR];
	if (ramfs_too_long(path)) {
		return -ZOS_EINVAL;
	}
	if (ramfs_equal(path, "/")) {
		return -ZOS_EEXIST;
	}
	if (ramfs_find(in, path) != (struct ramfs_node *)0) {
		return -ZOS_EEXIST;
	}
	if (!ramfs_parent_is_dir(in, path)) {
		return -ZOS_ENOENT;
	}
	node = ramfs_alloc_node(in);
	if (node == (struct ramfs_node *)0) {
		return -ZOS_ENOSPC;
	}
	node->used = 1;
	node->type = ZFS_TYPE_DIR;
	node->offset = 0;
	node->size = 0;
	ramfs_copy(node->path, path);
	ramfs_split(path, ramfs_scratch, ramfs_name);
	ramfs_apply_control(in, ramfs_name);
	return 0;
}

static int ramfs_rmdir(void *fs, const char *path)
{
	struct ramfs_instance *in = (struct ramfs_instance *)fs;
	struct ramfs_node *node;

	++in->stats.calls[RAMFS_C_RMDIR];
	if (ramfs_too_long(path)) {
		return -ZOS_EINVAL;
	}
	node = ramfs_find(in, path);
	if (node == (struct ramfs_node *)0) {
		return -ZOS_ENOENT;
	}
	if (node->type != ZFS_TYPE_DIR) {
		return -ZOS_ENOTDIR;
	}
	if (ramfs_has_children(in, path)) {
		return -ZOS_ENOTEMPTY;
	}
	ramfs_split(path, ramfs_scratch, ramfs_name);
	if (ramfs_equal(ramfs_name, "STALE")) {
		in->stale = 0;
	}
	ramfs_node_reset(node);
	return 0;
}

static int ramfs_unlink(void *fs, const char *path)
{
	struct ramfs_instance *in = (struct ramfs_instance *)fs;
	struct ramfs_node *node;

	++in->stats.calls[RAMFS_C_UNLINK];
	if (ramfs_too_long(path)) {
		return -ZOS_EINVAL;
	}
	node = ramfs_find(in, path);
	if (node == (struct ramfs_node *)0) {
		return -ZOS_ENOENT;
	}
	if (node->type == ZFS_TYPE_DIR) {
		return -ZOS_EISDIR;
	}
	ramfs_node_reset(node);
	return 0;
}

static int ramfs_rename(void *fs, const char *oldpath, const char *newpath)
{
	struct ramfs_instance *in = (struct ramfs_instance *)fs;
	struct ramfs_node *node;
	uint16_t i;

	++in->stats.calls[RAMFS_C_RENAME];
	if (ramfs_too_long(oldpath) || ramfs_too_long(newpath)) {
		return -ZOS_EINVAL;
	}
	node = ramfs_find(in, oldpath);
	if (node == (struct ramfs_node *)0) {
		return -ZOS_ENOENT;
	}
	if (ramfs_find(in, newpath) != (struct ramfs_node *)0) {
		return -ZOS_EEXIST;
	}
	if (!ramfs_parent_is_dir(in, newpath)) {
		return -ZOS_ENOENT;
	}
	for (i = 0; i < RAMFS_NODES; ++i) {
		struct ramfs_node *n = &in->nodes[i];
		const char *tail;

		if (!n->used) {
			continue;
		}
		if (ramfs_equal(n->path, oldpath)) {
			ramfs_copy(n->path, newpath);
			continue;
		}
		if (!ramfs_prefix(n->path, oldpath)) {
			continue;
		}
		tail = n->path + ramfs_length(oldpath);
		ramfs_make_path(ramfs_rebuilt, newpath, tail);
		ramfs_copy(n->path, ramfs_rebuilt);
	}
	return 0;
}

static const struct zfs_ops ramfs_ops = {
	ramfs_mount,
	ramfs_open,
	ramfs_close,
	ramfs_read,
	ramfs_write,
	ramfs_sync,
	ramfs_stat,
	ramfs_opendir,
	ramfs_readdir,
	ramfs_closedir,
	ramfs_mkdir,
	ramfs_rmdir,
	ramfs_unlink,
	ramfs_rename
};

static const struct zfs_provider ramfs_provider = {
	"ramfs",
	&ramfs_ops,
	(void *)0
};

int ramfs_init(const struct zdriver_api *api)
{
	return api->fs_register(&ramfs_provider);
}
