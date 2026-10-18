#ifndef TEST_KOS_H
#define TEST_KOS_H

#include <stdint.h>
#include <stdio.h>

#define KOS_EINVAL 2
#define KOS_EIO 5
#define KFS_TYPE_FILE 1u

struct kfs_stat {
	uint32_t size;
	unsigned char type;
};

int stat(const char *path, struct kfs_stat *info);

#endif
