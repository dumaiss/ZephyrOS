/* Host shim for the sound oracles (tests/snd/oracle): the minimal
 * KonaOS surface the two reference players include, mapped onto the host
 * C library.  The real headers live in user/sdk/include and abi/kos;
 * these stubs let the ZTR oracle's sources (ztr.c/player.c/psg.c)
 * build untouched on the host with a plain hosted compiler. */

#ifndef SND_ORACLE_KOS_H
#define SND_ORACLE_KOS_H

#include <stdint.h>
#include <stdio.h>

#define KOS_EINVAL  0x02
#define KOS_ENODEV  0x03
#define KOS_EBUSY   0x04
#define KOS_ENOTSUP 0x07
#define KOS_EIO     0x0A

#define KFS_TYPE_FILE 1u

struct kfs_stat {
	uint32_t size;
	unsigned char type;
};

int stat(const char *path, struct kfs_stat *info);

#endif /* SND_ORACLE_KOS_H */
