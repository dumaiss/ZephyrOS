/* KonaOS nucleus: the real-time interrupt (architecture 4.12).
 *
 * The IO controller's RT clock pulses the Z80's NMI.  An NMI fetches from
 * 0066h of whatever bank is mapped at 0000h, so 0066h-0068h of every bank
 * always holds JP KOS_RT_VECTOR: written here at boot, by exec into each
 * child, and kept in an auxiliary bank by kos_aux_copy, the one path that
 * maps such a bank at 0000h.  An object that covers those three bytes
 * keeps them in rt_entry_store instead, so objects still start at 0000h.
 * The vector in common memory is a JP whose target is the owner's handler
 * or KOS_RT_IDLE, a lone RETN; switching it is one 16-bit store, so an NMI
 * between two instructions always finds a whole address.
 *
 * One program owns the interrupt at a time.  Its registration goes with
 * the process; the RT clock's own hold is the IO controller driver's
 * (`/dev/rtclock`), which the same exit releases.  Nothing here runs on
 * the tick: the handler is entered straight from the vector.
 *
 * On Mk2 (KOS_BOARD_MK2) the entry lives in page-0 images only (2.7.6):
 * the bootstrap writes the system page-0 image's and exec each child's,
 * and no copy maps an object's page at 0000h, so there is no per-bank
 * write and no auxiliary-bank store.
 */

#include "kos/errno.h"
#include "kos/rt.h"
#include "memory.h"
#include "process.h"
#include "kernel/syscall/diag.h"
#include "kernel/syscall/rt.h"

/* The owner: pid + 1, or 0. */
static uint8_t rt_owner;

#if !defined(KOS_BOARD_MK2)
/* The NMI entry's three bytes in an auxiliary bank. */
#define RT_ENTRY_END   ((uint16_t)(KOS_NMI_ENTRY + 3u))
#define RT_AUX_BANKS   6u                       /* banks 0-5 (P = 2-7) */
#define XMEM_WINDOW_LO 0x6000u                  /* the mode-11 window  */

/* An object's own bytes at 0066h-0068h of each auxiliary bank. */
static uint8_t rt_entry_store[RT_AUX_BANKS][3];
#endif

_Static_assert(KOS_RT_VECTOR == 0xEF08u && KOS_RT_IDLE == 0xEF0Bu,
	       "rt_route stores the target at KOS_RT_VECTOR + 1 (EF09h)");

/* Point the vector at `target` with one instruction (HL, sdcccall(1)). */
static void rt_route(uint16_t target) __naked
{
	(void)target;
	__asm
		ld	(0xEF09),hl
		ret
	__endasm;
}

void kos_rt_boot(void)
{
	volatile uint8_t *vector = (volatile uint8_t *)KOS_RT_VECTOR;
#if !defined(KOS_BOARD_MK2)
	uint8_t bank;
#endif

	vector[0] = 0xC3u;                           /* JP KOS_RT_IDLE */
	vector[1] = (uint8_t)KOS_RT_IDLE;
	vector[2] = (uint8_t)(KOS_RT_IDLE >> 8);
	vector[3] = 0xEDu;                           /* RETN           */
	vector[4] = 0x45u;
	rt_owner = 0u;
#if !defined(KOS_BOARD_MK2)
	for (bank = 0u; bank < 7u; ++bank) {
		(void)kos_rt_page_zero(bank);
	}
#endif
}

#if !defined(KOS_BOARD_MK2)

uint8_t kos_rt_page_zero(uint8_t bank)
{
	struct kos_bank_copy_req req;

	kos_user_copy_staging[0] = 0xC3u;            /* JP KOS_RT_VECTOR */
	kos_user_copy_staging[1] = (uint8_t)KOS_RT_VECTOR;
	kos_user_copy_staging[2] = (uint8_t)(KOS_RT_VECTOR >> 8);
	req.bank = bank;
	req.op = KOS_BANK_OP_WRITE;
	req.addr = KOS_NMI_ENTRY;
	req.buffer = kos_user_copy_staging;
	req.length = 3u;
	return kos_bank_copy(&req);
}

uint8_t kos_aux_copy(const struct kos_bank_copy_req *req)
{
	struct kos_bank_copy_req part;
	uint16_t addr = req->addr;
	uint16_t end = (uint16_t)(addr + req->length);
	uint16_t at;
	uint8_t *store;

	if (addr >= XMEM_WINDOW_LO) {
		return kos_window_copy(req);
	}
	if (req->bank >= RT_AUX_BANKS || end <= KOS_NMI_ENTRY ||
	    addr >= RT_ENTRY_END) {
		return kos_bank_copy(req);
	}
	part = *req;
	if (addr < KOS_NMI_ENTRY) {
		part.length = (uint16_t)(KOS_NMI_ENTRY - addr);
		if (kos_bank_copy(&part) != 0u) {
			return 1u;
		}
	}
	store = rt_entry_store[req->bank];
	for (at = addr < KOS_NMI_ENTRY ? KOS_NMI_ENTRY : addr;
	     at < end && at < RT_ENTRY_END; ++at) {
		uint8_t *cell = &store[at - KOS_NMI_ENTRY];

		if (req->op == KOS_BANK_OP_READ) {
			req->buffer[at - addr] = *cell;
		} else if (req->op == KOS_BANK_OP_WRITE) {
			*cell = req->buffer[at - addr];
		} else {
			*cell = 0u;
		}
	}
	if (end > RT_ENTRY_END) {
		part.addr = RT_ENTRY_END;
		part.buffer = req->buffer + (RT_ENTRY_END - addr);
		part.length = (uint16_t)(end - RT_ENTRY_END);
		return kos_bank_copy(&part);
	}
	return 0u;
}
#endif /* !KOS_BOARD_MK2 */

void kos_rt_release_process(uint8_t pid)
{
	if (rt_owner != 0u && rt_owner == (uint8_t)(pid + 1u)) {
		rt_route(KOS_RT_IDLE);
		rt_owner = 0u;
	}
}

#if defined(KOS_BOARD_MK2)
void kos_rt_quiesce(void)
{
	rt_route(KOS_RT_IDLE);
	rt_owner = 0u;
}
#endif

void kos_rt_register(uint16_t block)
{
	const struct kos_rt_register_args *args =
		(const struct kos_rt_register_args *)block;
	uint8_t me = (uint8_t)(process_current_pid() + 1u);
	uint16_t handler;

	if (!kos_syscall_block_ok(block, sizeof(*args))) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	handler = args->handler;
	if (handler < KOS_RT_REGION_BASE ||
	    handler >= (uint16_t)(KOS_RT_REGION_BASE + KOS_RT_REGION_SIZE)) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	if (rt_owner != 0u && rt_owner != me) {
		kos_syscall_error(KOS_EBUSY);
		return;
	}
	rt_owner = me;
	rt_route(handler);
	kos_syscall_success(0);
}

void kos_rt_release(void)
{
	kos_rt_release_process(process_current_pid());
	kos_syscall_success(0);
}

void kos_rt_info(uint16_t block)
{
	const struct kos_rt_info_args *args =
		(const struct kos_rt_info_args *)block;
	uint8_t *record = kos_user_copy_staging;
	uint8_t me = (uint8_t)(process_current_pid() + 1u);

	if (!kos_syscall_block_ok(block, sizeof(*args)) ||
	    !kos_syscall_buffer_ok(args->buffer,
				   (uint16_t)sizeof(struct kos_rt_info))) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	record[0] = (uint8_t)KOS_RT_REGION_BASE;
	record[1] = (uint8_t)(KOS_RT_REGION_BASE >> 8);
	record[2] = (uint8_t)KOS_RT_REGION_SIZE;
	record[3] = (uint8_t)(KOS_RT_REGION_SIZE >> 8);
	record[4] = rt_owner == 0u ? KOS_RT_FREE
		: (rt_owner == me ? KOS_RT_MINE : KOS_RT_OTHER);
	record[5] = 0u;
	record[6] = 0u;
	record[7] = 0u;
	if (kos_syscall_bank_write(args->buffer,
				   (uint16_t)sizeof(struct kos_rt_info)) != 0u) {
		kos_syscall_error(KOS_EFAULT);
		return;
	}
	kos_syscall_success(0);
}
