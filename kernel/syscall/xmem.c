/* ZephyrOS M10 extended-memory syscall services (package 33c).
 *
 * Implements SYS_XMEM_ALLOC / _FREE / _READ / _WRITE / _INFO and
 * SYS_ZEX_SEGMENT_INFO (architecture section 36.7).  Runtime objects are
 * process-owned, zero-initialized, read/write and non-executable, and
 * are placed by the unified bank map into 8-KiB auxiliary slots.  Every
 * object is one opaque handle; a normal program is never told the
 * physical bank or slot.
 *
 * All transfers reuse the M6 checked copy gate; every range is validated
 * before a byte moves.  Failure leaves no state behind.
 */

#include "zos/syscall.h"
#include "zephyros/process.h"
#include "kernel/process/bankmap.h"
#include "kernel/process/zex2prog.h"
#include "xmem.h"
#include "diag.h"

#define XMEM_MAX_OBJECTS 8u
#define XMEM_SLOT_SIZE   ((uint16_t)ZEX2_AUX_SLOT_SIZE)
#define XMEM_MAX_BYTES   ((uint32_t)XMEM_SLOT_SIZE * ZOS_AUX_SLOT_COUNT)

struct xmem_obj {
	uint8_t  used;
	uint8_t  pid;
	uint8_t  bank;
	uint8_t  slot;
	uint8_t  slots;
	uint8_t  type;
	uint16_t bytes;
	uint16_t requested;
};

static struct xmem_obj xmem_table[XMEM_MAX_OBJECTS];

extern uint8_t zos_user_copy_staging[];

static uint16_t xmem_le16(uint16_t addr)
{
	const uint8_t *p = (const uint8_t *)addr;

	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static struct xmem_obj *xmem_lookup(uint16_t handle, uint8_t pid)
{
	uint8_t index;

	if (handle == 0 || handle > XMEM_MAX_OBJECTS) {
		return (struct xmem_obj *)0;
	}
	index = (uint8_t)(handle - 1u);
	if (!xmem_table[index].used || xmem_table[index].pid != pid) {
		return (struct xmem_obj *)0; /* stale or foreign */
	}
	return &xmem_table[index];
}

static uint8_t xmem_zero(uint8_t bank, uint16_t addr, uint16_t length)
{
	struct zos_bank_copy_req req;

	req.bank = bank;
	req.op = ZOS_BANK_OP_ZERO;
	req.addr = addr;
	req.buffer = (uint8_t *)0;
	req.length = length;
	return zos_bank_copy(&req);
}

static uint16_t xmem_slots_for(uint16_t size)
{
	uint16_t slots = (uint16_t)((size + XMEM_SLOT_SIZE - 1u) /
				    XMEM_SLOT_SIZE);

	return slots == 0 ? 1u : slots;
}

void zos_xmem_alloc(uint8_t bank, uint16_t block)
{
	struct xmem_obj *obj;
	uint16_t size;
	uint16_t flags;
	uint16_t slots;
	uint16_t bytes;
	uint8_t aux_bank;
	uint8_t slot;
	uint8_t pid = process_current_pid();
	uint8_t index;

	(void)bank;
	if (!zos_syscall_block_ok(block, 4) || pid >= PROCESS_TABLE_SIZE) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	size = xmem_le16(block);
	flags = xmem_le16((uint16_t)(block + 2));
	if (size == 0 || flags != 0) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	if ((uint32_t)size > XMEM_MAX_BYTES) {
		zos_syscall_error(ZOS_ENOSPC);
		return;
	}
	for (index = 0; index < XMEM_MAX_OBJECTS; ++index) {
		if (!xmem_table[index].used) {
			break;
		}
	}
	if (index == XMEM_MAX_OBJECTS) {
		zos_syscall_error(ZOS_ENOSPC);
		return;
	}
	slots = xmem_slots_for(size);
	bytes = (uint16_t)(slots * XMEM_SLOT_SIZE);
	if (bankmap_aux_alloc(pid, (uint8_t)slots, &aux_bank, &slot) != 0) {
		zos_syscall_error(ZOS_ENOSPC);
		return;
	}
	if (xmem_zero(aux_bank, bankmap_slot_base(slot), bytes) != 0) {
		bankmap_aux_free(pid, aux_bank, slot, (uint8_t)slots);
		zos_syscall_error(ZOS_EIO);
		return;
	}
	obj = &xmem_table[index];
	obj->used = 1;
	obj->pid = pid;
	obj->bank = aux_bank;
	obj->slot = slot;
	obj->slots = (uint8_t)slots;
	obj->type = ZOS_XMEM_TYPE_RUNTIME;
	obj->bytes = bytes;
	obj->requested = size;
	zos_syscall_success((uint16_t)(index + 1u));
}

void zos_xmem_free(uint8_t bank, uint16_t block)
{
	struct xmem_obj *obj;
	uint8_t pid = process_current_pid();

	(void)bank;
	if (!zos_syscall_block_ok(block, 2)) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	obj = xmem_lookup(xmem_le16(block), pid);
	if (obj == (struct xmem_obj *)0) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	bankmap_aux_free(obj->pid, obj->bank, obj->slot, obj->slots);
	obj->used = 0;
	zos_syscall_success(0);
}

/* One transfer chunk against a far object, through the common gate. */
static uint8_t xmem_obj_read(const struct xmem_obj *obj, uint16_t offset,
			     uint16_t length)
{
	struct zos_bank_copy_req req;

	req.bank = obj->bank;
	req.op = ZOS_BANK_OP_READ;
	req.addr = (uint16_t)(bankmap_slot_base(obj->slot) + offset);
	req.buffer = zos_user_copy_staging;
	req.length = length;
	return zos_bank_copy(&req);
}

static uint8_t xmem_obj_write(const struct xmem_obj *obj, uint16_t offset,
			      uint16_t length)
{
	struct zos_bank_copy_req req;

	req.bank = obj->bank;
	req.op = ZOS_BANK_OP_WRITE;
	req.addr = (uint16_t)(bankmap_slot_base(obj->slot) + offset);
	req.buffer = zos_user_copy_staging;
	req.length = length;
	return zos_bank_copy(&req);
}

static void xmem_transfer(uint8_t bank, uint16_t block, int to_object)
{
	struct xmem_obj *obj;
	uint16_t offset;
	uint16_t buffer;
	uint16_t length;
	uint16_t done = 0;
	uint8_t pid = process_current_pid();

	if (!zos_syscall_block_ok(block, 8)) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	obj = xmem_lookup(xmem_le16(block), pid);
	if (obj == (struct xmem_obj *)0) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	offset = xmem_le16((uint16_t)(block + 2));
	buffer = xmem_le16((uint16_t)(block + 4));
	length = xmem_le16((uint16_t)(block + 6));
	if (length == 0) {
		zos_syscall_success(0);
		return;
	}
	if (!zos_syscall_buffer_ok(buffer, length)) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	/* Checked in 32 bits before any copy: object range and caller range
	 * are both proven, so no chunk can partly succeed. */
	if ((uint32_t)offset + length > obj->bytes) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	while (done < length) {
		uint16_t chunk = (uint16_t)(length - done);

		if (chunk > ZOS_SYSCALL_STAGE_SIZE) {
			chunk = ZOS_SYSCALL_STAGE_SIZE;
		}
		if (to_object) {
			if (zos_syscall_bank_read(
				    bank, (uint16_t)(buffer + done),
				    chunk) != 0 ||
			    xmem_obj_write(obj, (uint16_t)(offset + done),
					   chunk) != 0) {
				zos_syscall_error(ZOS_EINVAL);
				return;
			}
		} else {
			if (xmem_obj_read(obj, (uint16_t)(offset + done),
					  chunk) != 0 ||
			    zos_syscall_bank_write(
				    bank, (uint16_t)(buffer + done),
				    chunk) != 0) {
				zos_syscall_error(ZOS_EINVAL);
				return;
			}
		}
		done = (uint16_t)(done + chunk);
	}
	zos_syscall_success(done);
}

void zos_xmem_read(uint8_t bank, uint16_t block)
{
	xmem_transfer(bank, block, 0);
}

void zos_xmem_write(uint8_t bank, uint16_t block)
{
	xmem_transfer(bank, block, 1);
}

void zos_xmem_info(uint8_t bank, uint16_t block)
{
	struct xmem_obj *obj;
	uint16_t buffer;
	uint8_t *record = zos_user_copy_staging;
	uint8_t pid = process_current_pid();

	if (!zos_syscall_block_ok(block, 4)) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	obj = xmem_lookup(xmem_le16(block), pid);
	buffer = xmem_le16((uint16_t)(block + 2));
	if (obj == (struct xmem_obj *)0 ||
	    !zos_syscall_buffer_ok(buffer, 8)) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	record[0] = (uint8_t)(obj->requested & 0xFFu);
	record[1] = (uint8_t)(obj->requested >> 8);
	record[2] = (uint8_t)(obj->bytes & 0xFFu);
	record[3] = (uint8_t)(obj->bytes >> 8);
	record[4] = 0;
	record[5] = 0;
	record[6] = ZOS_XMEM_TYPE_RUNTIME;
	record[7] = 0;
	if (zos_syscall_bank_write(bank, buffer, 8) != 0) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	zos_syscall_success(8);
}

void zos_zex_segment_info(uint8_t bank, uint16_t block)
{
	uint8_t segment_id;
	uint16_t buffer;
	uint8_t *record = zos_user_copy_staging;
	uint8_t pid = process_current_pid();
	uint8_t i;

	if (!zos_syscall_block_ok(block, 4)) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	segment_id = ((const uint8_t *)block)[0];
	buffer = xmem_le16((uint16_t)(block + 2));
	if (!zos_syscall_buffer_ok(buffer, 8)) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	if (zos_zex2_owner != (uint8_t)(pid + 1u) ||
	    pid >= PROCESS_TABLE_SIZE) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	for (i = 0; i < zos_zex2_seg_count; ++i) {
		if (!zos_zex2_segs[i].present ||
		    zos_zex2_segs[i].id != segment_id) {
			continue;
		}
		record[0] = (uint8_t)(zos_zex2_segs[i].image_size & 0xFFu);
		record[1] = (uint8_t)(zos_zex2_segs[i].image_size >> 8);
		record[2] = (uint8_t)(zos_zex2_segs[i].bss_size & 0xFFu);
		record[3] = (uint8_t)(zos_zex2_segs[i].bss_size >> 8);
		record[4] = zos_zex2_segs[i].flags;
		record[5] = ZOS_XMEM_SEG_PRESENT;
		record[6] = 0;
		record[7] = 0;
		if (zos_syscall_bank_write(bank, buffer, 8) != 0) {
			zos_syscall_error(ZOS_EINVAL);
			return;
		}
		zos_syscall_success(8);
		return;
	}
	zos_syscall_error(ZOS_ENOENT);
}

void zos_xmem_release_pid(uint8_t pid)
{
	uint8_t i;

	for (i = 0; i < XMEM_MAX_OBJECTS; ++i) {
		if (xmem_table[i].used && xmem_table[i].pid == pid) {
			bankmap_aux_free(pid, xmem_table[i].bank,
					 xmem_table[i].slot,
					 xmem_table[i].slots);
			xmem_table[i].used = 0;
		}
	}
}

void zos_xmem_dispatch(uint8_t number, uint8_t bank, uint16_t block)
{
	switch (number) {
	case ZOS_SYSCALL_XMEM_ALLOC:
		zos_xmem_alloc(bank, block);
		break;
	case ZOS_SYSCALL_XMEM_FREE:
		zos_xmem_free(bank, block);
		break;
	case ZOS_SYSCALL_XMEM_READ:
		zos_xmem_read(bank, block);
		break;
	case ZOS_SYSCALL_XMEM_WRITE:
		zos_xmem_write(bank, block);
		break;
	case ZOS_SYSCALL_XMEM_INFO:
		zos_xmem_info(bank, block);
		break;
	case ZOS_SYSCALL_ZEX_SEGMENT_INFO:
		zos_zex_segment_info(bank, block);
		break;
	case ZOS_SYSCALL_FAR_CALL:
		zos_far_call(bank, block);
		break;
	default:
		zos_syscall_error(ZOS_ENOSYS);
		break;
	}
}
