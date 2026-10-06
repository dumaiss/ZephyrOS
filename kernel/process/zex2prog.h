/* ZephyrOS M10 ZEX v2 resident program state (design spike, 33b).
 *
 * The loader records what a loaded ZEX v2 process owns so the runtime
 * services can resolve a logical id without re-reading the container.
 * Only one process is RUNNING at a time, so a single program-state block
 * tagged with its owner pid is enough; the loader resets it on every
 * load and the exit/rollback path clears it.
 *
 * Compiled only under ZOS_M10_SPIKE.  33c/33d add the syscall services
 * that consume this state.
 */

#ifndef ZEPHYROS_ZEX2PROG_H
#define ZEPHYROS_ZEX2PROG_H

#include "zos/types.h"
#include "zephyros/zex2.h"

#define ZEX2_PROG_MAX_SEGMENTS ZEX2_MAX_SEGMENTS
#define ZEX2_PROG_MAX_FAR      ZEX2_MAX_FAR_ENTRIES

struct zex2_seg_state {
	uint8_t  present;
	uint8_t  id;
	uint8_t  flags;
	uint8_t  bank;
	uint8_t  slot;
	uint8_t  slot_count;
	uint16_t image_size;
	uint16_t bss_size;
};

struct zex2_far_state {
	uint8_t  present;
	uint8_t  id;
	uint8_t  segment; /* index into zos_zex2_segs */
	uint16_t offset;
};

extern struct zex2_seg_state zos_zex2_segs[ZEX2_PROG_MAX_SEGMENTS];
extern struct zex2_far_state zos_zex2_fars[ZEX2_PROG_MAX_FAR];
extern uint8_t zos_zex2_seg_count;
extern uint8_t zos_zex2_far_count;
extern uint8_t zos_zex2_owner; /* pid + 1, 0 = no program */

void zex2_prog_reset(void);
void zex2_prog_set_owner(uint8_t pid);

#endif /* ZEPHYROS_ZEX2PROG_H */
