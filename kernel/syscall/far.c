/* ZephyrOS M10 SYS_FAR_CALL service (package 33d).
 *
 * Validates a far-entry id, prepares the auxiliary low window (page zero,
 * RST 30h vector, far stack and the bounded in-frame), enters the far
 * entry through the common gate (kernel/process/far.asm) and copies the
 * out-frame back.  One cross-bank far call at a time per process:
 * a recursive SYS_FAR_CALL returns ZOS_EBUSY (architecture 36.15).
 *
 * The gate reuses the package-28 continuation idea, so ordinary syscalls
 * issued by the far code work: the syscall gate's single-depth transition
 * state is shadowed for the duration of the call.  The dispatcher must
 * copy user buffers from the *execution* bank (the fix in
 * kernel/syscall.c), which is the far segment's bank while far code runs.
 */

#include "zos/syscall.h"
#include "zephyros/process.h"
#include "kernel/process/bankmap.h"
#include "kernel/process/zex2prog.h"
#include "xmem.h"
#include "diag.h"

#define FAR_FRAME_BASE 0x0100u /* in the auxiliary low window */
#define FAR_WINDOW_LEN 0x2000u /* auxiliary low window 0000h-1FFFh */

struct zos_far_entry_req {
	uint8_t  bank;
	uint16_t entry;
	uint16_t frame;
};

extern void zos_far_entry(const struct zos_far_entry_req *req);
extern uint16_t zos_far_result;
extern const uint8_t zos_process_page_zero_vector[];
extern uint8_t zos_user_copy_staging[];

static uint8_t far_active;

uint8_t zos_far_is_active(void)
{
	return far_active;
}

static uint16_t far_le16(uint16_t addr)
{
	const uint8_t *p = (const uint8_t *)addr;

	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint8_t far_zero(uint16_t addr, uint16_t length, uint8_t bank)
{
	struct zos_bank_copy_req req;

	req.bank = bank;
	req.op = ZOS_BANK_OP_ZERO;
	req.addr = addr;
	req.buffer = (uint8_t *)0;
	req.length = length;
	return zos_bank_copy(&req);
}

static uint8_t far_write(uint16_t addr, const uint8_t *src, uint16_t length,
			 uint8_t bank)
{
	struct zos_bank_copy_req req;

	req.bank = bank;
	req.op = ZOS_BANK_OP_WRITE;
	req.addr = addr;
	req.buffer = (uint8_t *)src;
	req.length = length;
	return zos_bank_copy(&req);
}

/* Copy `length` bytes from the caller bank into the far window, chunked. */
static uint8_t far_copy_in(uint8_t caller_bank, uint16_t src,
			   uint16_t dst, uint16_t length, uint8_t far_bank)
{
	uint16_t done = 0;

	while (done < length) {
		uint16_t chunk = (uint16_t)(length - done);

		if (chunk > ZOS_SYSCALL_STAGE_SIZE) {
			chunk = ZOS_SYSCALL_STAGE_SIZE;
		}
		if (zos_syscall_bank_read(caller_bank,
					  (uint16_t)(src + done),
					  chunk) != 0) {
			return 1;
		}
		if (far_write((uint16_t)(dst + done), zos_user_copy_staging,
			      chunk, far_bank) != 0) {
			return 1;
		}
		done = (uint16_t)(done + chunk);
	}
	return 0;
}

/* Copy `length` bytes from the far window back into the caller bank. */
static uint8_t far_copy_out(uint8_t far_bank, uint16_t src,
			    uint8_t caller_bank, uint16_t dst,
			    uint16_t length)
{
	uint16_t done = 0;

	while (done < length) {
		uint16_t chunk = (uint16_t)(length - done);
		struct zos_bank_copy_req req;

		if (chunk > ZOS_SYSCALL_STAGE_SIZE) {
			chunk = ZOS_SYSCALL_STAGE_SIZE;
		}
		req.bank = far_bank;
		req.op = ZOS_BANK_OP_READ;
		req.addr = (uint16_t)(src + done);
		req.buffer = zos_user_copy_staging;
		req.length = chunk;
		if (zos_bank_copy(&req) != 0) {
			return 1;
		}
		if (zos_syscall_bank_write(caller_bank,
					   (uint16_t)(dst + done),
					   chunk) != 0) {
			return 1;
		}
		done = (uint16_t)(done + chunk);
	}
	return 0;
}

void zos_far_call(uint8_t bank, uint16_t block)
{
	struct zos_far_entry_req req;
	uint16_t in;
	uint16_t in_len;
	uint16_t out;
	uint16_t out_len;
	uint16_t target = 0;
	uint8_t entry_id;
	uint8_t far_bank = 0;
	uint8_t found = 0xFF;
	uint8_t pid = process_current_pid();
	uint8_t i;

	if (!zos_syscall_block_ok(block, 10) ||
	    pid >= PROCESS_TABLE_SIZE) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	entry_id = ((const uint8_t *)block)[0];
	in = far_le16((uint16_t)(block + 2));
	in_len = far_le16((uint16_t)(block + 4));
	out = far_le16((uint16_t)(block + 6));
	out_len = far_le16((uint16_t)(block + 8));
	if (((const uint8_t *)block)[1] != 0 ||
	    in_len > ZOS_FAR_FRAME_MAX || out_len > ZOS_FAR_FRAME_MAX) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	if ((in_len != 0 && !zos_syscall_buffer_ok(in, in_len)) ||
	    (out_len != 0 && !zos_syscall_buffer_ok(out, out_len))) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	if (far_active) {
		zos_syscall_error(ZOS_EBUSY); /* recursive far call */
		return;
	}
	if (zos_zex2_owner != (uint8_t)(pid + 1u)) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	for (i = 0; i < zos_zex2_far_count; ++i) {
		if (zos_zex2_fars[i].present &&
		    zos_zex2_fars[i].id == entry_id) {
			found = zos_zex2_fars[i].segment;
			target = (uint16_t)(bankmap_slot_base(
						    zos_zex2_segs[found].slot) +
					    zos_zex2_fars[i].offset);
			far_bank = zos_zex2_segs[found].bank;
			break;
		}
	}
	if (found == 0xFF) {
		zos_syscall_error(ZOS_ENOENT);
		return;
	}

	/* Prepare the auxiliary low window: page zero with the RST 30h
	 * vector, the far stack (the gate sets SP in this window) and the
	 * in-frame. */
	if (far_zero(0x0000, FAR_WINDOW_LEN, far_bank) != 0 ||
	    far_write(0x0030, zos_process_page_zero_vector, 3, far_bank) != 0 ||
	    (in_len != 0 &&
	     far_copy_in(bank, in, FAR_FRAME_BASE, in_len, far_bank) != 0)) {
		zos_syscall_error(ZOS_EIO);
		return;
	}

	req.bank = far_bank;
	req.entry = target;
	req.frame = FAR_FRAME_BASE;
	far_active = 1;
	zos_far_entry(&req);
	far_active = 0;

	if (out_len != 0 &&
	    far_copy_out(far_bank, FAR_FRAME_BASE, bank, out, out_len) != 0) {
		zos_syscall_error(ZOS_EIO);
		return;
	}
	zos_syscall_success(zos_far_result);
}
