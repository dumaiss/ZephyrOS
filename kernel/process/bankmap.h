/* ZephyrOS M10 unified application-bank ownership (design spike, 33b).
 *
 * One allocator owns every application bank (architecture section 36.5).
 * A bank 0-6 is FREE, PRIMARY(pid) (the process table owns it whole) or
 * AUXILIARY(pid) (it carries one or more 8-KiB allocation slots).  The
 * primary side stays with kernel/process/process.c; this module adds the
 * auxiliary side and answers "may this bank become a primary bank?" so
 * the two never disagree.
 *
 * Compiled only under ZOS_M10_SPIKE.
 */

#ifndef ZEPHYROS_BANKMAP_H
#define ZEPHYROS_BANKMAP_H

#include "zos/types.h"
#include "zephyros/process.h"
#include "zephyros/zex2.h"

#define ZOS_AUX_SLOT_COUNT ZEX2_AUX_SLOT_COUNT

/* Allocate `slots` contiguous auxiliary slots owned by pid.  Packs an
 * existing auxiliary bank of the same pid first, then takes the highest
 * completely free bank (architecture section 36.5).  Returns 0 and writes
 * the chosen bank and slot through the out pointers, or -ZOS_ENOSPC. */
int bankmap_aux_alloc(uint8_t pid, uint8_t slots, uint8_t *bank,
		      uint8_t *slot);

/* Release every auxiliary slot owned by pid (process exit/rollback). */
void bankmap_aux_release(uint8_t pid);

/* Release one auxiliary allocation (SYS_XMEM_FREE): `slots` slots starting
 * at `slot` in `bank`, owned by pid.  Frees the bank when it empties. */
void bankmap_aux_free(uint8_t pid, uint8_t bank, uint8_t slot,
		      uint8_t slots);

/* 1 when the bank carries no auxiliary allocation and may become a
 * primary bank. */
uint8_t bankmap_bank_free(uint8_t bank);

/* Logical base of a slot inside its auxiliary bank. */
uint16_t bankmap_slot_base(uint8_t slot);

#endif /* ZEPHYROS_BANKMAP_H */
