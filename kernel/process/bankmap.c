/* ZephyrOS M10 unified application-bank ownership (design spike, 33b).
 *
 * See kernel/process/bankmap.h.  The auxiliary side is one owner byte per
 * bank (pid + 1, 0 = FREE) and one 6-bit slot bitmap per bank; a bank is
 * AUXILIARY when its owner is non-zero.
 */

#include "kernel/process/bankmap.h"
#include "zephyros/errno.h"

uint8_t zos_aux_owner[PROCESS_BANK_COUNT];
uint8_t zos_aux_slots[PROCESS_BANK_COUNT];

uint16_t bankmap_slot_base(uint8_t slot)
{
	return (uint16_t)(ZEX2_AUX_SLOT_BASE +
			  (uint16_t)slot * ZEX2_AUX_SLOT_SIZE);
}

uint8_t bankmap_bank_free(uint8_t bank)
{
	if (bank >= PROCESS_BANK_COUNT) {
		return 0;
	}
	return zos_aux_owner[bank] == 0 ? 1u : 0u;
}

/* Lowest index of a run of `run` clear bits. */
static int bank_slot_run(uint8_t bank, uint8_t run)
{
	uint8_t start = 0;

	while ((uint8_t)(start + run) <= ZOS_AUX_SLOT_COUNT) {
		uint8_t i;
		uint8_t ok = 1;

		for (i = 0; i < run; ++i) {
			if (((zos_aux_slots[bank] >> (start + i)) & 1u) != 0) {
				ok = 0;
				break;
			}
		}
		if (ok) {
			return (int)start;
		}
		++start;
	}
	return -1;
}

int bankmap_aux_alloc(uint8_t pid, uint8_t slots, uint8_t *bank,
		      uint8_t *slot)
{
	uint8_t owner = (uint8_t)(pid + 1u);
	uint8_t b;

	if (slots == 0 || slots > ZOS_AUX_SLOT_COUNT) {
		return -ZOS_ENOSPC;
	}
	/* Pack an existing auxiliary bank of the same process first. */
	for (b = 0; b < PROCESS_BANK_COUNT; ++b) {
		int start;

		if (zos_aux_owner[b] != owner) {
			continue;
		}
		start = bank_slot_run(b, slots);
		if (start >= 0) {
			*bank = b;
			*slot = (uint8_t)start;
			zos_aux_slots[b] |= (uint8_t)(((1u << slots) - 1u)
						      << start);
			return 0;
		}
	}
	/* Otherwise take the highest completely free bank. */
	for (b = PROCESS_BANK_COUNT; b-- > 0;) {
		if (zos_aux_owner[b] != 0) {
			continue;
		}
		if (process_bank_primary_used(b)) {
			continue; /* reserved or a live primary bank */
		}
		zos_aux_owner[b] = owner;
		zos_aux_slots[b] = (uint8_t)((1u << slots) - 1u);
		*bank = b;
		*slot = 0;
		return 0;
	}
	return -ZOS_ENOSPC;
}

void bankmap_aux_release(uint8_t pid)
{
	uint8_t owner = (uint8_t)(pid + 1u);
	uint8_t b;

	for (b = 0; b < PROCESS_BANK_COUNT; ++b) {
		if (zos_aux_owner[b] == owner) {
			zos_aux_owner[b] = 0;
			zos_aux_slots[b] = 0;
		}
	}
}

void bankmap_aux_free(uint8_t pid, uint8_t bank, uint8_t slot,
		      uint8_t slots)
{
	uint8_t mask;

	if (bank >= PROCESS_BANK_COUNT ||
	    zos_aux_owner[bank] != (uint8_t)(pid + 1u)) {
		return;
	}
	if (slots >= ZOS_AUX_SLOT_COUNT) {
		mask = 0x3F;
	} else {
		mask = (uint8_t)(((1u << slots) - 1u) << slot);
	}
	zos_aux_slots[bank] &= (uint8_t)~mask;
	if (zos_aux_slots[bank] == 0) {
		zos_aux_owner[bank] = 0;
	}
}
