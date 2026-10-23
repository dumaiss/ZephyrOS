#include "kos.h"

#include "ke_platform.h"

int ke_xmem_alloc(uint16_t size, uint16_t flags)
{
	return xalloc(size, flags);
}

int ke_xmem_free(uint16_t handle)
{
	return xfree(handle);
}

int ke_xmem_read(uint16_t handle, uint16_t offset, void *buffer,
		 uint16_t length)
{
	return xread(handle, offset, buffer, length);
}

int ke_xmem_write(uint16_t handle, uint16_t offset, const void *buffer,
		  uint16_t length)
{
	return xwrite(handle, offset, buffer, length);
}

int ke_fs_open(const char *path, uint8_t flags)
{
	return open(path, flags);
}

int ke_fs_close(uint16_t handle)
{
	return close(handle);
}

int ke_fs_read(uint16_t handle, void *buffer, uint16_t length)
{
	return read(handle, buffer, length);
}

int ke_fs_write(uint16_t handle, const void *buffer, uint16_t length)
{
	return write(handle, buffer, length);
}

int ke_fs_sync(uint16_t handle)
{
	return sync(handle);
}

int ke_fs_stat(const char *path, struct ke_file_info *info)
{
	struct kfs_stat native;
	int result = stat(path, &native);

	if (result >= 0) {
		info->type = native.type;
		info->size = native.size;
	}
	return result;
}

int ke_fs_unlink(const char *path)
{
	return unlink(path);
}

int ke_fs_rename(const char *old_path, const char *new_path)
{
	return rename(old_path, new_path);
}

int ke_fs_list(const char *path, uint16_t *cookie,
	       struct ke_dir_entry *entries, uint16_t count)
{
	struct kos_ns_entry native[8];
	uint16_t request = count > 8u ? 8u : count;
	int result;
	uint16_t i;

	result = list(path, cookie, native, request);
	if (result <= 0) return result;
	for (i = 0u; i < (uint16_t)result; ++i) {
		entries[i].type = native[i].type;
		entries[i].container =
			(uint8_t)((native[i].flags & KOS_NS_ENTRY_CONTAINER) != 0u);
		memcpy(entries[i].name, native[i].name, sizeof(entries[i].name));
	}
	return result;
}
