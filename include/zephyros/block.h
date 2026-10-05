/* ZephyrOS generic block-device class contract (M9, driver ABI v3).
 *
 * ZDEV_BLOCK is the raw block-I/O class: a driver exposes media geometry
 * and fixed-size block transfers, and a filesystem provider may layer on
 * top of it.  Class value, structure layouts, capability bits and
 * operation order are fixed by docs/architecture.md section 32.15 and
 * docs/devices.md.
 *
 * M9 publishes this contract only.  No block driver is resident, because
 * nothing consumes one: IOC-FAT talks to ZDEV_IOC directly.  The first
 * block driver arrives with its first consumer (the first native
 * filesystem); the future SD backend is `sd-ioc.zmod`, a ZDEV_BLOCK
 * driver that consumes a ZDEV_IOC transport.
 *
 * Capabilities are authoritative: the device's capability mask states
 * which operations exist, and the matching `ZCAP_BLOCK_*` bit gates each
 * operation.  `get_info` is required; the remaining pointers are null
 * when their capability bit is absent and are then never called.
 *
 * Results are 0 on success or a negative `ZOS_E*` on failure.  In
 * particular `media_status` returns 0 when the media is present and
 * usable, `-ZOS_ENODEV` when there is no media, and `-ZOS_EIO` when the
 * media is degraded.
 */

#ifndef ZEPHYROS_BLOCK_H
#define ZEPHYROS_BLOCK_H

#include "zos/types.h"
#include "zephyros/errno.h"
#include "zephyros/device.h"

/* Media geometry and transfer limits reported by `get_info`.
 *
 *   block_size           bytes in one addressable block
 *   block_count          number of addressable blocks on the media
 *   max_transfer_blocks  largest block count accepted by one read/write
 *   flags                reserved; zero
 *
 * `lba` addresses the first block of a transfer in units of
 * `block_size`.  A read or write of `count` blocks must satisfy
 * `count <= max_transfer_blocks`. */
struct zblock_info {
	uint16_t block_size;
	uint32_t block_count;
	uint16_t max_transfer_blocks;
	uint16_t flags;
};

/* Class operations, in the architecture's fixed order.  A null pointer
 * means the matching capability bit is absent. */
struct zblock_ops {
	int (*get_info)(void *state, struct zblock_info *out);
	int (*read)(void *state, uint32_t lba, uint16_t count, void *buffer);
	int (*write)(void *state, uint32_t lba, uint16_t count,
		     const void *buffer);
	int (*flush)(void *state);
	int (*media_status)(void *state);
};

#if defined(__SDCC)
/* Pin the packed layout so a compiler/flag drift cannot silently break
 * the class ABI. */
_Static_assert(sizeof(struct zblock_info) == 10,
	       "zblock_info must be 10 packed bytes");
_Static_assert(__builtin_offsetof(struct zblock_info, block_count) == 2,
	       "zblock_info block_count offset");
_Static_assert(__builtin_offsetof(struct zblock_info, max_transfer_blocks)
	       == 6, "zblock_info max_transfer_blocks offset");
_Static_assert(__builtin_offsetof(struct zblock_info, flags) == 8,
	       "zblock_info flags offset");
_Static_assert(sizeof(struct zblock_ops) == 10,
	       "zblock_ops must be 10 packed bytes");
_Static_assert(__builtin_offsetof(struct zblock_ops, read) == 2,
	       "zblock_ops read offset");
_Static_assert(__builtin_offsetof(struct zblock_ops, write) == 4,
	       "zblock_ops write offset");
_Static_assert(__builtin_offsetof(struct zblock_ops, flush) == 6,
	       "zblock_ops flush offset");
_Static_assert(__builtin_offsetof(struct zblock_ops, media_status) == 8,
	       "zblock_ops media_status offset");
#endif /* __SDCC */

#endif /* ZEPHYROS_BLOCK_H */
