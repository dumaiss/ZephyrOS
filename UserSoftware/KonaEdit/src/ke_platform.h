#ifndef KONAEDIT_PLATFORM_H
#define KONAEDIT_PLATFORM_H

#include "ke_types.h"

#define KE_XMEM_NONE     0u
#define KE_XMEM_MAPPABLE 1u

#define KE_FS_READ   0x01u
#define KE_FS_WRITE  0x02u
#define KE_FS_CREATE 0x04u
#define KE_FS_EXCL   0x08u
#define KE_FS_TRUNC  0x10u

#define KE_PATH_MAX 128u

struct ke_file_info {
	uint8_t type;
	uint32_t size;
};

struct ke_dir_entry {
	uint8_t type;
	uint8_t container;
	char name[24];
};

int ke_xmem_alloc(uint16_t size, uint16_t flags);
int ke_xmem_free(uint16_t handle);
int ke_xmem_read(uint16_t handle, uint16_t offset, void *buffer,
		 uint16_t length);
int ke_xmem_write(uint16_t handle, uint16_t offset, const void *buffer,
		  uint16_t length);
int ke_fs_open(const char *path, uint8_t flags);
int ke_fs_close(uint16_t handle);
int ke_fs_read(uint16_t handle, void *buffer, uint16_t length);
int ke_fs_write(uint16_t handle, const void *buffer, uint16_t length);
int ke_fs_sync(uint16_t handle);
int ke_fs_stat(const char *path, struct ke_file_info *info);
int ke_fs_unlink(const char *path);
int ke_fs_rename(const char *old_path, const char *new_path);
int ke_fs_list(const char *path, uint16_t *cookie,
	       struct ke_dir_entry *entries, uint16_t count);

#endif
