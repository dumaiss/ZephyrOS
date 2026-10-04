/* ZephyrOS M9 VFS mount registry, runtime state and boot lifecycle.
 *
 * Mount policy comes only from the generated `const` table
 * (kernel/vfs/mount_config.h + build/generated/vfs_mounts.c).  VFS keeps a
 * parallel `_DATA` runtime array and never writes the configuration.  The
 * built-in provider registration table below is the kernel's half of the
 * build-time/registry "built-in provider membership" invariant; the host
 * test keeps it in sync with the Makefile's ZOS_VFS_BUILTIN.
 *
 * Lifecycle (architecture section 32.32):
 *   vfs_init()      register built-ins, mount the unique system entry
 *   (module phase)  modules may register providers
 *   vfs_mount_all() instantiate the remaining entries
 *   READY           namespace fixed
 *
 * Mount selection is flat (architecture sections 32.6 and 32.8): the first
 * path component is compared case-insensitively with the mounted root-level
 * mountpoints, otherwise the filesystem attached at `/` owns the path.  No
 * longest-match search, no node objects, no per-component walk.
 */

#include "kernel/vfs/vfs.h"
#include "zephyros/kernel.h"
#include "fs/romfs/romfs_provider.h"

/* Built-in provider registration table: the providers statically linked
 * into the kernel image and available before any module loads. */
static const struct zfs_provider *const vfs_builtin_providers[] = {
	&romfs_provider
};
#define VFS_BUILTIN_COUNT \
	((uint8_t)(sizeof(vfs_builtin_providers) / \
		   sizeof(vfs_builtin_providers[0])))

struct vfs_mount_runtime vfs_mount_state[VFS_MAX_MOUNTS];

static const struct zfs_provider *vfs_providers[VFS_PROVIDER_MAX];
static uint8_t vfs_providers_count;
static uint8_t vfs_current_phase = VFS_PHASE_BOOTSTRAP;

/* --- provider registry ------------------------------------------------ */

int vfs_register_filesystem(const struct zfs_provider *provider)
{
	uint16_t i;

	if (provider == (const struct zfs_provider *)0 ||
	    provider->name == (const char *)0 ||
	    provider->name[0] == '\0' ||
	    provider->ops == (const struct zfs_ops *)0) {
		return -ZOS_EINVAL;
	}
	if (vfs_current_phase == VFS_PHASE_MOUNT ||
	    vfs_current_phase == VFS_PHASE_READY) {
		return -ZOS_EBUSY;
	}
	for (i = 0; i < vfs_providers_count; ++i) {
		if (vfs_name_equal(vfs_providers[i]->name, provider->name)) {
			return -ZOS_EBUSY;
		}
	}
	if (vfs_providers_count >= VFS_PROVIDER_MAX) {
		return -ZOS_ENOSPC;
	}
	vfs_providers[vfs_providers_count++] = provider;
	return 0;
}

const struct zfs_provider *vfs_find_provider(const char *name)
{
	uint16_t i;

	if (name == (const char *)0) {
		return (const struct zfs_provider *)0;
	}
	for (i = 0; i < vfs_providers_count; ++i) {
		if (vfs_name_equal(vfs_providers[i]->name, name)) {
			return vfs_providers[i];
		}
	}
	return (const struct zfs_provider *)0;
}

uint8_t vfs_phase(void)
{
	return vfs_current_phase;
}

/* --- diagnostics ------------------------------------------------------ */

static void vfs_diag_unavailable(const char *path, int result)
{
	static const char hex[] = "0123456789ABCDEF";
	unsigned code = (unsigned)(-result) & 0xFFu;

	early_console_puts("vfs: ");
	early_console_puts(path);
	early_console_puts(" unavailable 0x");
	early_console_putc(hex[(code >> 4) & 0x0Fu]);
	early_console_putc(hex[code & 0x0Fu]);
	early_console_puts("\r\n");
}

/* The single shared bank-7 pathname scratch buffer (architecture section
 * 32.10).  `rename` is the only operation that needs a second one. */
static char vfs_norm[ZOS_PATH_MAX];

/* --- mount table helpers ---------------------------------------------- */

/* Architecture section 32.6 restricts configured mountpoints to `/` or one
 * root component, so "root-level" is a property of the configured path and
 * not a per-component walk: `/` qualifies and any path without a further
 * `/` qualifies. */
uint8_t vfs_mount_is_root_level(uint16_t index)
{
	const char *path = vfs_mount_table[index].path;
	uint8_t i = 1;

	if (path[0] != '/') {
		return 0;
	}
	while (path[i] != '\0') {
		if (path[i] == '/') {
			return 0;
		}
		++i;
	}
	return 1;
}

uint8_t vfs_mount_is_root(uint16_t index)
{
	return (uint8_t)(vfs_mount_table[index].path_len == 1u &&
			 vfs_mount_table[index].path[0] == '/');
}

/* ASCII-folded comparison of the first `length` bytes of two names
 * (architecture section 32.8: VFS folds only the first component). */
static uint8_t vfs_first_component_equal(const char *name, const char *path,
					 uint8_t length)
{
	uint8_t i;

	for (i = 0; i < length; ++i) {
		uint8_t a = (uint8_t)name[i];
		uint8_t b = (uint8_t)path[i];

		if (a >= 'a' && a <= 'z') {
			a = (uint8_t)(a - 32);
		}
		if (b >= 'a' && b <= 'z') {
			b = (uint8_t)(b - 32);
		}
		if (a != b) {
			return 0;
		}
	}
	return 1;
}

int vfs_mount_resolve(const char *norm, uint16_t *index, const char **rel)
{
	const char *first;
	uint8_t length = 0;
	uint16_t i;

	if (norm == (const char *)0 || norm[0] != '/' ||
	    index == (uint16_t *)0 || rel == (const char **)0) {
		return -ZOS_EINVAL;
	}
	first = norm + 1;
	while (first[length] != '\0' && first[length] != '/') {
		++length;
	}
	for (i = 0; i < vfs_mount_count; ++i) {
		const struct vfs_mount_config *cfg = &vfs_mount_table[i];

		if (vfs_mount_state[i].state != VFS_MOUNT_MOUNTED ||
		    !vfs_mount_is_root_level(i) || vfs_mount_is_root(i)) {
			continue;
		}
		if ((uint8_t)(cfg->path_len - 1u) != length) {
			continue;
		}
		if (!vfs_first_component_equal(first, cfg->path + 1, length)) {
			continue;
		}
		*rel = (first[length] == '/') ? &first[length] : "/";
		*index = i;
		return 0;
	}
	for (i = 0; i < vfs_mount_count; ++i) {
		if (vfs_mount_state[i].state == VFS_MOUNT_MOUNTED &&
		    vfs_mount_is_root(i)) {
			*index = i;
			*rel = norm;
			return 0;
		}
	}
	return -ZOS_ENODEV;
}

uint8_t vfs_is_mountpoint(const char *norm)
{
	uint16_t i;

	if (norm == (const char *)0 || norm[0] != '/') {
		return 0;
	}
	if (norm[1] == '\0') {
		return 1; /* the VFS root itself */
	}
	for (i = 0; i < vfs_mount_count; ++i) {
		if (vfs_mount_state[i].state != VFS_MOUNT_MOUNTED ||
		    !vfs_mount_is_root_level(i)) {
			continue;
		}
		if (vfs_name_equal(vfs_mount_table[i].path, norm)) {
			return 1;
		}
	}
	return 0;
}

/* Normalize `path` and select its owning mount, then report the provider
 * path in *rel.  Returns VFS_RESOLVED_PROVIDER, VFS_RESOLVED_ROOT (the path
 * is `/`), VFS_RESOLVED_MOUNTPOINT (the path is one of the mounted
 * root-level mountpoints, so *rel is `/`), or a negative ZOS_E* value. */
int vfs_resolve_path(const char *path, uint16_t *index, const char **rel)
{
	int result;

	result = vfs_path_normalize(path, vfs_norm, ZOS_PATH_MAX);
	if (result != 0) {
		return result;
	}
	if (vfs_norm[1] == '\0') {
		return VFS_RESOLVED_ROOT;
	}
	result = vfs_mount_resolve(vfs_norm, index, rel);
	if (result != 0) {
		return result;
	}
	if (vfs_is_mountpoint(vfs_norm)) {
		*rel = "/";
		return VFS_RESOLVED_MOUNTPOINT;
	}
	return VFS_RESOLVED_PROVIDER;
}

/* --- lifecycle -------------------------------------------------------- */

static void vfs_mount_state_reset(void)
{
	uint8_t i;

	for (i = 0; i < VFS_MAX_MOUNTS; ++i) {
		vfs_mount_state[i].state = VFS_MOUNT_UNMOUNTED;
		vfs_mount_state[i].reserved = 0;
		vfs_mount_state[i].provider = (const struct zfs_provider *)0;
		vfs_mount_state[i].fs_state = (void *)0;
		vfs_mount_state[i].last_error = 0;
	}
}

static int vfs_mount_one(uint8_t index)
{
	const struct vfs_mount_config *cfg = &vfs_mount_table[index];
	const struct zfs_provider *provider;
	void *fs_state = (void *)0;
	int result;

	provider = vfs_find_provider(cfg->filesystem);
	if (provider == (const struct zfs_provider *)0) {
		vfs_mount_state[index].state = VFS_MOUNT_FAILED;
		vfs_mount_state[index].last_error = -ZOS_ENODEV;
		return -ZOS_ENODEV;
	}
	result = provider->ops->mount(provider->provider_state, cfg->source,
				      &fs_state);
	if (result != 0) {
		vfs_mount_state[index].state = VFS_MOUNT_FAILED;
		vfs_mount_state[index].last_error = result;
		return result;
	}
	vfs_mount_state[index].state = VFS_MOUNT_MOUNTED;
	vfs_mount_state[index].provider = provider;
	vfs_mount_state[index].fs_state = fs_state;
	vfs_mount_state[index].last_error = 0;
	return 0;
}

int vfs_init(void)
{
	uint16_t i;
	uint8_t system_count = 0;
	uint8_t system_index = 0;
	int result;

	vfs_current_phase = VFS_PHASE_BOOTSTRAP;
	vfs_providers_count = 0;
	vfs_mount_state_reset();

	if (vfs_mount_count > VFS_MAX_MOUNTS) {
		panic("vfs: mount table overflow");
	}
	i = 0;
	while (i < VFS_BUILTIN_COUNT) {
		result = vfs_register_filesystem(vfs_builtin_providers[i]);
		if (result != 0) {
			return result;
		}
		++i;
	}
	i = 0;
	while (i < vfs_mount_count) {
		if ((vfs_mount_table[i].flags & VFS_MOUNT_SYSTEM) != 0) {
			++system_count;
			system_index = i;
		}
		++i;
	}
	if (system_count != 1u) {
		panic("vfs: system mount missing");
	}
	result = vfs_mount_one(system_index);
	if (result != 0) {
		return result;
	}
	vfs_current_phase = VFS_PHASE_MODULES;
	return 0;
}

int vfs_mount_all(void)
{
	uint8_t i;

	if (vfs_current_phase != VFS_PHASE_MODULES) {
		return -ZOS_EINVAL;
	}
	vfs_current_phase = VFS_PHASE_MOUNT;
	for (i = 0; i < vfs_mount_count; ++i) {
		const struct vfs_mount_config *cfg = &vfs_mount_table[i];
		int result;

		if ((cfg->flags & VFS_MOUNT_SYSTEM) != 0) {
			continue;
		}
		result = vfs_mount_one(i);
		if (result == 0) {
			continue;
		}
		/* Required failures are fatal too; package 26 refines the
		 * required-mount wording.  The optional diagnostic is the
		 * pinned `vfs: <path> unavailable 0xNN` line. */
		vfs_diag_unavailable(cfg->path, result);
		if ((cfg->flags & VFS_MOUNT_REQUIRED) != 0) {
			panic("vfs: mount failed");
		}
	}
	vfs_current_phase = VFS_PHASE_READY;
	return 0;
}
