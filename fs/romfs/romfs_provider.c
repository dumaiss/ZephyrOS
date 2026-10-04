/* ZephyrOS M9 ROMFS filesystem provider.
 *
 * Exposes the proven M5 recovery-store reader (kernel/romfs) through the
 * path-based `zfs_ops` provider ABI (architecture sections 32.12 and
 * 32.13).  The provider is statically linked into the kernel image and
 * mounted at `/SYSTEM` by VFS bootstrap; it contains no on-ROM layout
 * knowledge beyond the reader's validated metadata and no Zephyr-80 mount
 * policy.
 *
 * Semantics (work package 25, correction-02):
 *   - `/` is the only directory; `/<name>` is a file;
 *   - a deeper path is ZOS_ENOTDIR when its first component names a file
 *     and ZOS_ENOENT when it names nothing;
 *   - names match with the reader's single ASCII case-folded comparison;
 *   - every file is read-only, so the mutation pointers are null and VFS
 *     answers ZOS_EROFS (architecture section 32.12);
 *   - a missing name is ZOS_ENOENT, write flags are ZOS_EROFS;
 *   - the VFS_O_EXEC hint accepts only ZEX entries, preserving the M6
 *     executable-media rejection;
 *   - the only accepted source is "recovery" (else ZOS_EINVAL).
 *
 * Open handles come from a small static pool (there is no dynamic
 * allocation); exhaustion reports ZOS_EMFILE -- the provider open-handle
 * exhaustion the native error is defined for.  A slot holds a file's
 * validated entry metadata or a directory's iteration cursor.
 */

#include "fs/romfs/romfs_provider.h"
#include "kernel/vfs/vfs.h"
#include "zephyros/romfs.h"

/* Everything this file defines is either the exported `romfs_provider`
 * record or a `romfs_pv_*` private, so a build can be checked for "no
 * ROMFS-provider symbol" with one prefix (tools/mapcheck.py). */

/* One pool serves files and directories.  Two simultaneous opens are all
 * the M9 boot path needs (a module/ZEX source plus a directory probe). */
#define ROMFS_SLOT_MAX 2u

struct romfs_slot {
	uint32_t offset; /* payload offset (file)        */
	uint32_t size;   /* payload size (file)          */
	uint16_t cursor; /* next entry index (directory) */
	uint16_t flags;  /* ROMFS_FLAG_* (file)          */
	uint8_t used;
};

static struct romfs_slot romfs_pv_pool[ROMFS_SLOT_MAX];

static uint8_t romfs_pv_source_ok(const char *source)
{
	static const char wanted[] = "recovery";
	uint8_t i;

	if (source == (const char *)0) {
		return 0;
	}
	for (i = 0; i < (uint8_t)(sizeof(wanted)); ++i) {
		if (source[i] != wanted[i]) {
			return 0;
		}
		if (wanted[i] == '\0') {
			return 1;
		}
	}
	return 0;
}

/* --- handle pool ------------------------------------------------------ */

/* Instantiate the provider instance.  The pool is cleared here rather than
 * relying on the C runtime having zeroed `_DATA`: mount is the only way to
 * obtain an instance, so this makes the provider correct however the image
 * was entered (a partial replay that calls kernel_main() without
 * __zos_crt0_init() must behave like the real boot). */
static void romfs_pv_reset(void)
{
	uint8_t i;

	for (i = 0; i < ROMFS_SLOT_MAX; ++i) {
		romfs_pv_pool[i].used = 0;
		romfs_pv_pool[i].cursor = 0;
	}
}

static struct romfs_slot *romfs_pv_slot_alloc(void)
{
	uint8_t i;

	for (i = 0; i < ROMFS_SLOT_MAX; ++i) {
		if (romfs_pv_pool[i].used == 0) {
			romfs_pv_pool[i].used = 1;
			romfs_pv_pool[i].cursor = 0;
			return &romfs_pv_pool[i];
		}
	}
	return (struct romfs_slot *)0;
}

/* The handle always came from this pool (VFS only passes back what a
 * provider handed it), but a stale or foreign pointer must still be
 * refused rather than followed. */
static struct romfs_slot *romfs_pv_slot_get(void *handle)
{
	struct romfs_slot *slot = (struct romfs_slot *)handle;

	if (slot < &romfs_pv_pool[0] || slot >= &romfs_pv_pool[ROMFS_SLOT_MAX] ||
	    slot->used == 0) {
		return (struct romfs_slot *)0;
	}
	return slot;
}

static int romfs_op_close(void *fs, void *file)
{
	struct romfs_slot *slot = romfs_pv_slot_get(file);

	if (fs == (void *)0 || slot == (struct romfs_slot *)0) {
		return -ZOS_EINVAL;
	}
	slot->used = 0;
	return 0;
}

/* --- provider path handling ------------------------------------------- */

static char romfs_pv_component[ROMFS_NAME_SIZE];

/* Resolve a provider-relative path.  Sets *entry for `/<name>` and returns
 * 0; returns -ZOS_EISDIR for `/`; a deeper path follows the directory rule:
 * -ZOS_ENOTDIR when the first component names an existing file, else
 * -ZOS_ENOENT. */
static int romfs_pv_path_entry(const char *path, romfs_file_t *entry)
{
	const char *name;
	const char *scan;

	if (path == (const char *)0 || path[0] != '/') {
		return -ZOS_EINVAL;
	}
	name = path + 1;
	if (name[0] == '\0') {
		return -ZOS_EISDIR; /* `/` is the filesystem directory */
	}
	for (scan = name; *scan != '\0'; ++scan) {
		uint16_t n = 0;
		int result;

		if (*scan != '/') {
			continue;
		}
		/* A deeper path: name the first component and decide whether
		 * it is an existing file (ENOTDIR) or unknown (ENOENT).  A
		 * component longer than any ROMFS name cannot match. */
		while (name[n] != '/' && n + 1u < ROMFS_NAME_SIZE) {
			romfs_pv_component[n] = name[n];
			++n;
		}
		if (name[n] != '/') {
			return -ZOS_ENOENT;
		}
		romfs_pv_component[n] = '\0';
		result = romfs_lookup_ci(romfs_pv_component, entry);
		return (result == 0) ? -ZOS_ENOTDIR : result;
	}
	return romfs_lookup_ci(name, entry);
}

static void romfs_pv_fill_stat(const romfs_file_t *entry, struct zfs_stat *out)
{
	out->type = ZFS_TYPE_FILE;
	out->attributes = ZFS_ATTR_READONLY;
	out->reserved = 0;
	out->size = entry->size;
	out->ctime = ZFS_TIME_UNKNOWN;
	out->mtime = ZFS_TIME_UNKNOWN;
}

/* --- zfs_ops: instantiation ------------------------------------------- */

static int romfs_op_mount(void *provider_state, const char *source, void **fs)
{
	int result;

	(void)provider_state;
	if (fs == (void **)0) {
		return -ZOS_EINVAL;
	}
	if (!romfs_pv_source_ok(source)) {
		return -ZOS_EINVAL;
	}
	result = romfs_init();
	if (result != 0) {
		return result;
	}
	romfs_pv_reset();
	*fs = (void *)&romfs_pv_pool; /* opaque instance marker */
	return 0;
}

/* --- zfs_ops: files --------------------------------------------------- */

static int romfs_op_open(void *fs, const char *path, uint16_t flags,
			 void **file, uint32_t *size)
{
	struct romfs_slot *slot;
	romfs_file_t entry;
	int result;

	if (fs == (void *)0 || file == (void **)0 || size == (uint32_t *)0) {
		return -ZOS_EINVAL;
	}
	/* ROMFS is read-only: refuse any write intent before resolving. */
	if ((flags & (uint16_t)(ZFS_O_WRITE | ZFS_O_CREATE | ZFS_O_EXCL |
				ZFS_O_TRUNC)) != 0) {
		return -ZOS_EROFS;
	}
	result = romfs_pv_path_entry(path, &entry);
	if (result != 0) {
		return result;
	}
	if ((flags & VFS_O_EXEC) != 0 && entry.flags != ROMFS_FLAG_ZEX) {
		return -ZOS_EINVAL; /* not executable media */
	}
	slot = romfs_pv_slot_alloc();
	if (slot == (struct romfs_slot *)0) {
		return -ZOS_EMFILE;
	}
	slot->offset = entry.offset;
	slot->size = entry.size;
	slot->flags = entry.flags;
	*file = (void *)slot;
	*size = entry.size;
	return 0;
}

static int romfs_op_read(void *fs, void *file, uint32_t offset, void *buf,
			 uint16_t len)
{
	struct romfs_slot *slot = romfs_pv_slot_get(file);
	romfs_file_t entry;

	if (fs == (void *)0 || slot == (struct romfs_slot *)0) {
		return -ZOS_EINVAL;
	}
	entry.offset = slot->offset;
	entry.size = slot->size;
	entry.flags = slot->flags;
	return romfs_read(&entry, offset, buf, len);
}

static int romfs_op_stat(void *fs, const char *path, struct zfs_stat *out)
{
	romfs_file_t entry;
	int result;

	if (fs == (void *)0 || out == (struct zfs_stat *)0) {
		return -ZOS_EINVAL;
	}
	result = romfs_pv_path_entry(path, &entry);
	if (result == -ZOS_EISDIR) {
		out->type = ZFS_TYPE_DIR;
		out->attributes = ZFS_ATTR_READONLY;
		out->reserved = 0;
		out->size = 0;
		out->ctime = ZFS_TIME_UNKNOWN;
		out->mtime = ZFS_TIME_UNKNOWN;
		return 0;
	}
	if (result != 0) {
		return result;
	}
	romfs_pv_fill_stat(&entry, out);
	return 0;
}

/* --- zfs_ops: directories --------------------------------------------- */

static int romfs_op_opendir(void *fs, const char *path, void **dir)
{
	struct romfs_slot *slot;
	romfs_file_t entry;
	int result;

	if (fs == (void *)0 || dir == (void **)0) {
		return -ZOS_EINVAL;
	}
	result = romfs_pv_path_entry(path, &entry);
	if (result == 0) {
		return -ZOS_ENOTDIR; /* a file has no children */
	}
	if (result != -ZOS_EISDIR) {
		return result; /* deeper path or missing name */
	}
	slot = romfs_pv_slot_alloc();
	if (slot == (struct romfs_slot *)0) {
		return -ZOS_EMFILE;
	}
	*dir = (void *)slot;
	return 0;
}

static int romfs_op_readdir(void *fs, void *dir, struct zfs_dirent *out)
{
	struct romfs_slot *slot = romfs_pv_slot_get(dir);
	romfs_file_t entry;
	char name[ROMFS_NAME_SIZE];
	uint16_t n = 0;
	int result;

	if (fs == (void *)0 || slot == (struct romfs_slot *)0 ||
	    out == (struct zfs_dirent *)0) {
		return -ZOS_EINVAL;
	}
	result = romfs_entry_at(slot->cursor, &entry, name, ROMFS_NAME_SIZE);
	if (result != 0) {
		return (result == -ZOS_ENOENT) ? 0 : result;
	}
	++slot->cursor;
	while (n + 1u < ZFS_NAME_MAX && name[n] != '\0') {
		out->name[n] = name[n];
		++n;
	}
	out->name[n] = '\0';
	out->name_length = (uint16_t)(n + 1u);
	out->type = ZFS_TYPE_FILE;
	out->attributes = ZFS_ATTR_READONLY;
	out->size = entry.size;
	out->ctime = ZFS_TIME_UNKNOWN;
	out->mtime = ZFS_TIME_UNKNOWN;
	return 1;
}

/* --- registration ----------------------------------------------------- */

/* Read-only provider: the mutation pointers stay null and VFS answers
 * ZOS_EROFS (architecture section 32.12).  M9 has no unmount and no
 * filesystem-wide sync, so those operations do not exist. */
static const struct zfs_ops romfs_pv_ops = {
	romfs_op_mount,
	romfs_op_open,
	romfs_op_close,
	romfs_op_read,
	(void *)0, /* write  */
	(void *)0, /* sync   */
	romfs_op_stat,
	romfs_op_opendir,
	romfs_op_readdir,
	romfs_op_close, /* closedir */
	(void *)0, /* mkdir  */
	(void *)0, /* rmdir  */
	(void *)0, /* unlink */
	(void *)0, /* rename */
};

const struct zfs_provider romfs_provider = {
	"romfs",
	&romfs_pv_ops,
	(void *)0,
};
