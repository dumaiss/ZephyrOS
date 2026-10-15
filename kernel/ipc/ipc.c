/* KonaOS fixed-core IPC substrate (kernel/ipc, architecture 4.9).
 *
 * Everything here is compiled into the fixed core and executes, or is
 * touched, while the mode-11 window shows bank 6 (the resident driver
 * domain):
 *
 *   - the endpoint table: the host-assigned number, class/instance,
 *     version, dispatch entry, the owner's bulk extent, the re-entrancy
 *     flag and the busy mark left on the call stack.  The loader installs
 *     the host-built records (ipc_install_endpoint); there is no runtime
 *     registration path;
 *   - call with the fixed 32-byte request/reply frame.  The outermost
 *     call copies the frame into the fixed boundary buffer (needed for a
 *     caller frame in the cold half); a call from one resident driver to
 *     another is near-direct - the frame is passed by reference and the
 *     target's window is already open, so no copy and no bank switch
 *     happen (architecture 4.9);
 *   - nucleus-issued buffer descriptors captured at call entry from the
 *     syscall gate's saved execution latch, and the copy resolvers with
 *     bounds checked on every use.  The resolver picks one of three
 *     paths per request (architecture 4.9):
 *     A - caller window or common: one LDIR in mode 11, as promoted;
 *     B - the driver buffer is in bank 6's 8000h-BFFFh and the caller
 *         buffer is in the caller's own bank outside that range: one
 *         LDIR in mode 10 with P = 1 over the caller's bank, run from
 *         common memory (kernel/ipc/ipc_common.asm), aperture up;
 *     C - everything else, including a driver buffer outside
 *         8000h-BFFFh: chunked through the common-resident kos_bank_copy
 *         gate, counted as slow;
 *   - the nesting limit, re-entrancy refusal and the same-domain fast
 *     path;
 *   - the IRQ trampoline: the nucleus keeps the IM2 vectoring, the ISR
 *     stack and the final RETI; the driver handler runs in the owner's
 *     window on the nucleus ISR stack with the restricted API (no IPC).
 *
 * The loader that places driver images is not here: it is the cold
 * nucleus loader (kernel/driver/driver_loader.c), which reaches bank 6
 * through the upcall shim and the copy primitives, so no loader stays in
 * the fixed core.
 *
 * Single-tasking: the call depth, the descriptor table and the boundary
 * frame are nucleus state, serialised by the syscall gate and the
 * non-reentrant IRQ entry.
 */

#include "kos/ipc.h"
#include "kos/syscall.h"
#include "kos/errno.h"
#include "irq_types.h"
#include "kernel.h"
#include "platform.h"
#include "process.h"

#include "kernel/event/event.h"
#include "kernel/ipc/ipc.h"

#include "kernel/irq/irq_private.h"
#include "kernel/memory/upcall.h"
#include "kernel/syscall/diag.h"

/* Fixed-region primitives (kernel/ipc/ipc_asm.asm). */
extern uint8_t ipc_latch(void);
extern void ipc_domain_enter(uint8_t page_bits);
extern void ipc_domain_set(uint8_t page_bits);
extern void ipc_domain_exit(void);
extern uint16_t ipc_copy_req[3];
extern uint16_t ipc_copy_run(void);
/* Path-B primitive and its common request block (ipc_common.asm). */
extern uint16_t ipc_ap_copy(void);
extern uint8_t ipc_ap_req[7];
extern int16_t ipc_invoke(uint16_t entry, struct kos_ipc_frame *frame);
extern int16_t ipc_invoke_here(uint16_t entry, struct kos_ipc_frame *frame);

/* The common-resident application-bank copy gate and its staging buffer
 * (kernel/memory/user_copy.asm; declared with the syscall helpers). */
uint8_t kos_bank_copy(const struct kos_bank_copy_req *req);
extern uint8_t kos_user_copy_staging[];

/* Revision-13 page values (architecture 2.2): page 1 shows bank 6, the
 * resident driver domain; page 0 is the nucleus's own cold half.  The
 * upcall header owns these constants and ipc_asm.asm reads the same
 * platform constants. */
#define IPC_DRIVER_PAGE_BITS \
	(uint8_t)(KOS_UPCALL_PAGE_DRIVER << KOS_ROM_PAGE_SHIFT)
#define IPC_NUCLEUS_PAGE_BITS 0u
#define IPC_DRIVER_BANK (KOS_BANK_MASK - KOS_UPCALL_PAGE_DRIVER)

/* The mode-10 far aperture (architecture 7.2): with a nonzero page the
 * caller's 8000h-BFFFh names bank 7 - P, not the primary bank. */
#define IPC_APERTURE_BASE 0x8000u
#define IPC_APERTURE_END  0xC000u

/* Path B installs mode 10 with the caller's bank and page 1 (architecture
 * 2.2: page 1 shows bank 6).  Applications never receive a token for
 * that page; the aperture is nucleus-internal. */
#define IPC_APERTURE_PAGE_BITS IPC_DRIVER_PAGE_BITS

/* Driver I/O buffers (architecture 4.9): the sources
 * and targets a driver names in copy_in/copy_out - I/O buffers, caches,
 * staging - live in bank 6's 8000h-BFFFh, the one bank-6 range the
 * mode-11 window and the mode-10 aperture both show at the same address.
 * A driver address outside it is still copied, through the chunked
 * path, and counted as slow. */
#define IPC_DRIVER_IO_BASE IPC_APERTURE_BASE
#define IPC_DRIVER_IO_END  IPC_APERTURE_END

/* Frames and descriptors are bounded tables in fixed data.  The endpoint
 * table holds one record per endpoint of the image's whole boot set plus
 * the nucleus FILE provider: production declares six drivers and the IPC
 * acceptance image declares eight, so the capacity keeps headroom over
 * the largest set an image builds. */
#define IPC_ENDPOINT_MAX 12u
#define IPC_DESC_MAX 4u

struct ipc_endpoint {
	uint16_t dispatch; /* driver dispatch entry in the owner's domain */
	uint8_t class;
	uint8_t instance;
	uint8_t flags;
	uint8_t version;
	uint8_t number;    /* host-assigned endpoint number (1..N) */
	uint8_t busy;
	uint16_t bulk_base; /* the owner's bulk extent in bank 6 (5.2) */
	uint16_t bulk_size;
};

static struct ipc_endpoint endpoints[IPC_ENDPOINT_MAX];
static uint8_t endpoint_count;

/* The boot bundle the loader hands a driver's init (fixed core, so the
 * driver's window always shows it): the API table and the resolved
 * dependency numbers of the image being brought up. */
uint8_t kos_driver_deps[KOS_IPC_DRIVER_MAX_DEPS];

/* The endpoint being dispatched at each call level: api_bulk_read resolves
 * the calling driver from the innermost entry (drivers register one
 * bulk extent each). */
static const struct ipc_endpoint *call_owner[KOS_IPC_MAX_NESTING];

struct ipc_desc {
	uint8_t active;
	uint8_t dir;    /* KOS_IPC_DESC_IN / _OUT */
	uint8_t depth;  /* call level the descriptor belongs to */
	uint8_t latch;  /* caller execution latch (bank and page) */
	uint16_t addr;
	uint16_t length;
};

static struct ipc_desc descs[IPC_DESC_MAX];

/* The one nucleus-owned frame copy: the boundary buffer for the
 * outermost call, whose caller frame may live in bank 7's cold half (the
 * IOC-FAT class shim) or in the caller window (the syscall boundary).
 * Nested driver-to-driver calls pass their own frames by reference. */
static struct kos_ipc_frame call_frame;

static uint8_t call_depth;

/* Copy-path counters: one per resolver outcome, so the
 * acceptance replay can attribute every transfer to a path from the
 * outside.  Free-running 16-bit fixed data. */
uint16_t ipc_copy_window;
uint16_t ipc_copy_aperture;
uint16_t ipc_copy_slow;

/* IRQ ownership state: one driver handler per source, the granularity the
 * bootstrap interface documents (`irq_claim` claims *a* source).  The timer
 * driver owns two channels - its tick and its event timer - and each needs
 * its own in-image handler.  Every handler runs in bank 6, so the window
 * selection is one constant for all of them. */
static uint16_t irq_handler_entry[IRQ_SOURCE_COUNT];
static uint16_t irq_handler_context[IRQ_SOURCE_COUNT];
static uint8_t ipc_irq_active;

/* --- copy path helpers -------------------------------------------------- */

static uint8_t in_driver_window(uint16_t addr, uint16_t length)
{
	return (uint8_t)(length != 0u && addr >= KOS_REV13_WINDOW_BASE &&
			 (uint16_t)(addr + length) <= KOS_BANK7_RUNTIME_END);
}

static uint8_t in_caller_window(uint16_t addr, uint16_t length)
{
	return (uint8_t)(length != 0u && addr >= KOS_USER_BUFFER_BASE &&
			 addr < 0x2000u &&
			 (uint16_t)(addr + length) <= 0x2000u);
}

/* The permanent common window (E000h-FFFFh, physical bank 0): mapped at
 * the same address in every mode, so a caller buffer there is visible to
 * a path-A LDIR exactly like the caller window is. */
static uint8_t in_common(uint16_t addr, uint16_t length)
{
	return (uint8_t)(length != 0u && addr >= KOS_USER_BUFFER_END &&
			 (uint32_t)addr + length <= 0x10000u);
}

/* The driver's own I/O range: bank 6's 8000h-BFFFh. */
static uint8_t in_driver_io(uint16_t addr, uint16_t length)
{
	return (uint8_t)(length != 0u && addr >= IPC_DRIVER_IO_BASE &&
			 (uint16_t)(addr + length) <= IPC_DRIVER_IO_END);
}

/* A caller range that overlaps the aperture block cannot be shown while
 * the aperture is up: with P = 1 those addresses name bank 6, whatever
 * bank the caller's own decode puts there. */
static uint8_t crosses_aperture(uint16_t addr, uint16_t length)
{
	uint16_t end = (uint16_t)(addr + length);

	return (uint8_t)(length != 0u && addr < IPC_APERTURE_END &&
			 end > IPC_APERTURE_BASE);
}

/* The bank a descriptor's address resolves to.  The caller window and
 * everything outside the aperture stays in the caller's bank; inside the
 * mode-10 aperture with a nonzero page the far bank 7 - P is named
 * (architecture 4.9/7.2).  Descriptor capture refuses a buffer that
 * would cross the aperture boundary, so the whole descriptor resolves to
 * one bank.
 *
 * This is the only place a nonzero-page aperture address becomes a bank:
 * an aperture descriptor keeps its owner's bank, which forces the chunked
 * path C because that bank is not the caller's primary bank, and the
 * capture keeps the full latch (Q, mode, P) for it.  The syscall-side
 * copies resolve the same way (kernel/syscall.c
 * `kos_syscall_buffer_bank`). */
static uint8_t desc_bank(const struct ipc_desc *desc)
{
	uint8_t page = (uint8_t)((desc->latch & KOS_ROM_PAGE_MASK) >>
				 KOS_ROM_PAGE_SHIFT);

	if (page != 0u && desc->addr >= IPC_APERTURE_BASE &&
	    desc->addr < IPC_APERTURE_END) {
		return (uint8_t)(KOS_BANK_MASK - page);
	}
	return (uint8_t)(desc->latch & KOS_BANK_MASK);
}

static uint8_t bank_transfer(uint8_t bank, uint8_t op, uint16_t addr,
			     uint16_t length)
{
	struct kos_bank_copy_req req;

	req.bank = bank;
	req.op = op;
	req.addr = addr;
	req.buffer = kos_user_copy_staging;
	req.length = length;
	return kos_bank_copy(&req);
}

/* Chunked transfer through the common staging buffer, used by path C -
 * a caller buffer behind the aperture, an aperture-resolved far bank, or
 * a driver buffer outside 8000h-BFFFh.  The caller's bank is read and
 * written through kos_bank_copy; the driver side goes through the
 * window LDIR when it lies inside the mode-11 bank-6 range and through
 * kos_bank_copy(bank 6) otherwise. */
static int16_t copy_staged(uint8_t bank, uint16_t caller_addr,
			   uint16_t driver_addr, uint16_t length,
			   uint8_t dir)
{
	uint16_t done = 0u;

	while (done < length) {
		uint16_t chunk = (uint16_t)(length - done);
		uint16_t caller = (uint16_t)(caller_addr + done);
		uint16_t driver = (uint16_t)(driver_addr + done);

		if (chunk > KOS_SYSCALL_STAGE_SIZE) {
			chunk = KOS_SYSCALL_STAGE_SIZE;
		}
		if (dir == KOS_IPC_DESC_IN) {
			if (bank_transfer(bank, KOS_BANK_OP_READ, caller,
					  chunk) != 0u) {
				return -KOS_EINVAL;
			}
			if (in_driver_window(driver, chunk)) {
				ipc_copy_req[0] = (uint16_t)kos_user_copy_staging;
				ipc_copy_req[1] = driver;
				ipc_copy_req[2] = chunk;
				(void)ipc_copy_run();
			} else if (bank_transfer(IPC_DRIVER_BANK,
						 KOS_BANK_OP_WRITE, driver,
						 chunk) != 0u) {
				return -KOS_EINVAL;
			}
		} else {
			if (in_driver_window(driver, chunk)) {
				ipc_copy_req[0] = driver;
				ipc_copy_req[1] = (uint16_t)kos_user_copy_staging;
				ipc_copy_req[2] = chunk;
				(void)ipc_copy_run();
			} else if (bank_transfer(IPC_DRIVER_BANK,
						 KOS_BANK_OP_READ, driver,
						 chunk) != 0u) {
				return -KOS_EINVAL;
			}
			if (bank_transfer(bank, KOS_BANK_OP_WRITE, caller,
					  chunk) != 0u) {
				return -KOS_EINVAL;
			}
		}
		done = (uint16_t)(done + chunk);
	}
	return (int16_t)length;
}

/* One checked descriptor copy: the offset and length against the captured
 * descriptor, the direction against the capture, and the driver address
 * against the driver window (or the owner's bank through the copy gate).
 *
 * A single LDIR is only valid for a caller buffer in the mode-11 caller
 * window, so the resolver selects one of three paths per request
 * (architecture 4.9; see the module header). */
static int16_t copy_move(const struct ipc_desc *desc, uint16_t driver_addr,
			 uint16_t offset, uint16_t length, uint8_t dir)
{
	uint16_t caller_addr;
	uint8_t caller_bank;

	if (length == 0u || offset > desc->length ||
	    length > (uint16_t)(desc->length - offset)) {
		return -KOS_EINVAL;
	}
	caller_addr = (uint16_t)(desc->addr + offset);
	caller_bank = desc_bank(desc);
	if (in_driver_io(driver_addr, length)) {
		if (in_caller_window(caller_addr, length) ||
		    in_common(caller_addr, length)) {
			/* Path A: both sides visible in the driver
			 * mapping, which is mode 11 (one LDIR). */
			if (dir == KOS_IPC_DESC_IN) {
				ipc_copy_req[0] = caller_addr;
				ipc_copy_req[1] = driver_addr;
			} else {
				ipc_copy_req[0] = driver_addr;
				ipc_copy_req[1] = caller_addr;
			}
			ipc_copy_req[2] = length;
			++ipc_copy_window;
			return (int16_t)ipc_copy_run();
		}
		if (caller_bank == (uint8_t)(desc->latch & KOS_BANK_MASK) &&
		    caller_addr >= KOS_USER_BUFFER_BASE &&
		    (uint16_t)(caller_addr + length) <= KOS_USER_BUFFER_END &&
		    !crosses_aperture(caller_addr, length)) {
			/* Path B: the caller buffer is in the primary
			 * bank and clear of the aperture block, so mode 10
			 * over that bank with P = 1 shows the caller
			 * everywhere but 8000h-BFFFh, where it shows the
			 * driver's bank 6 - one LDIR, exactly restored. */
			ipc_ap_req[0] = (uint8_t)(KOS_MODE_APPLICATION |
						  IPC_APERTURE_PAGE_BITS |
						  caller_bank);
			if (dir == KOS_IPC_DESC_IN) {
				ipc_ap_req[1] = (uint8_t)caller_addr;
				ipc_ap_req[2] = (uint8_t)(caller_addr >> 8);
				ipc_ap_req[3] = (uint8_t)driver_addr;
				ipc_ap_req[4] = (uint8_t)(driver_addr >> 8);
			} else {
				ipc_ap_req[1] = (uint8_t)driver_addr;
				ipc_ap_req[2] = (uint8_t)(driver_addr >> 8);
				ipc_ap_req[3] = (uint8_t)caller_addr;
				ipc_ap_req[4] = (uint8_t)(caller_addr >> 8);
			}
			ipc_ap_req[5] = (uint8_t)length;
			ipc_ap_req[6] = (uint8_t)(length >> 8);
			++ipc_copy_aperture;
			return (int16_t)ipc_ap_copy();
		}
	}
	/* Path C: a caller buffer behind the aperture or outside the
	 * primary bank, an aperture-resolved far bank (P != 0 at capture,
	 * the far-bank resolution hook), or a driver buffer outside
	 * 8000h-BFFFh.  Chunked through the common-resident bank copy
	 * gate; counted as slow. */
	++ipc_copy_slow;
	return copy_staged(caller_bank, caller_addr, driver_addr, length,
			   dir);
}

/* --- descriptor capture and lookup -------------------------------------- */

static void desc_release_all(void)
{
	uint8_t index;

	for (index = 0u; index < IPC_DESC_MAX; ++index) {
		descs[index].active = 0u;
	}
}

/* Capture one caller buffer from the saved execution latch.  The buffer
 * window is 0100h-DFFFh (KOS_USER_BUFFER_BASE/END), the caller's own
 * bank, plus the common window E000h-FFFFh, which every mapping shows at
 * the same address (architecture 4.9); bank 7 can
 * never be a caller bank through the gate.  With a nonzero page, a
 * buffer may lie wholly inside the mode-10 aperture (the far bank) or
 * wholly outside it, never across its boundary. */
static int8_t desc_capture(uint8_t latch, uint16_t addr, uint16_t length,
			   uint8_t dir)
{
	uint8_t index;
	uint8_t page = (uint8_t)((latch & KOS_ROM_PAGE_MASK) >>
				 KOS_ROM_PAGE_SHIFT);
	uint16_t end;

	if (length == 0u || length > KOS_IPC_DESC_MAX || dir > 1u) {
		return (int8_t)-1;
	}
	if ((latch & KOS_BANK_MASK) >= KOS_BANK_MASK) {
		return (int8_t)-1;
	}
	if (addr < KOS_USER_BUFFER_BASE ||
	    (uint32_t)addr + length > 0x10000u) {
		return (int8_t)-1;
	}
	if (page != 0u) {
		end = (uint16_t)(addr + length);
		if (addr < IPC_APERTURE_END && end > IPC_APERTURE_BASE &&
		    !(addr >= IPC_APERTURE_BASE && end <= IPC_APERTURE_END)) {
			return (int8_t)-1;
		}
	}
	for (index = 0u; index < IPC_DESC_MAX; ++index) {
		if (!descs[index].active) {
			descs[index].active = 1u;
			descs[index].dir = dir;
			descs[index].depth = call_depth;
			descs[index].latch = latch;
			descs[index].addr = addr;
			descs[index].length = length;
			return (int8_t)index;
		}
	}
	return (int8_t)-1;
}

/* A descriptor id is valid only for calls deeper than its capture level:
 * the caller's ids stay live down the chain, and no driver can name a
 * descriptor of its own level or a forged id. */
static struct ipc_desc *desc_lookup(uint8_t id)
{
	if (id >= IPC_DESC_MAX) {
		return (struct ipc_desc *)0;
	}
	if (!descs[id].active || descs[id].depth >= call_depth) {
		return (struct ipc_desc *)0;
	}
	return &descs[id];
}

/* --- endpoint registration, lookup and dispatch -------------------------- */

/* The endpoint a handle names.  The handle is the host-assigned endpoint
 * number (architecture 5.3): the loader installs the records in number
 * order, so the slot is the number's index and the stored number confirms
 * the record. */
static struct ipc_endpoint *endpoint_for(uint8_t handle)
{
	/* The table also holds the in-process provider endpoints, whose
	 * reserved numbers live above the host range and break the
	 * "slot == number - 1" rule, so it is scanned by the stored
	 * number. */
	uint8_t index;

	if (handle == 0u) {
		return (struct ipc_endpoint *)0;
	}
	for (index = 0u; index < endpoint_count; ++index) {
		if (endpoints[index].number == handle) {
			return &endpoints[index];
		}
	}
	return (struct ipc_endpoint *)0;
}

uint8_t ipc_endpoint_flags(uint8_t number)
{
	const struct ipc_endpoint *ep = endpoint_for(number);

	return (ep != (const struct ipc_endpoint *)0) ? ep->flags : 0u;
}

/* Install one host-built endpoint record (architecture 5.3).  Only the
 * loader calls this, before any driver code runs; the checks protect the
 * nucleus table and the fixed driver window. */
int8_t ipc_install_endpoint(const struct ipc_endpoint_record *record)
{
	struct ipc_endpoint *ep;

	/* The loader already checked the record against the container; the
	 * checks here protect the nucleus table (capacity, uniqueness) and
	 * the call machinery (a dispatch entry inside the driver window).
	 * The table also holds the in-process provider endpoints, which are
	 * installed above the host range, so uniqueness - not
	 * "slot == number - 1" - is the invariant. */
	if (endpoint_count >= IPC_ENDPOINT_MAX ||
	    endpoint_for(record->number) != (struct ipc_endpoint *)0) {
		return (int8_t)(-KOS_ENOSPC);
	}
	if (record->entry < KOS_REV13_WINDOW_BASE ||
	    record->entry >= KOS_BANK7_RUNTIME_END) {
		return (int8_t)(-KOS_EINVAL);
	}
	ep = &endpoints[endpoint_count];
	ep->dispatch = record->entry;
	ep->class = record->class;
	ep->instance = record->instance;
	ep->flags = record->flags;
	ep->version = record->version;
	ep->number = record->number;
	ep->busy = 0u;
	ep->bulk_base = record->bulk_base;
	ep->bulk_size = record->bulk_size;
	++endpoint_count;
	return 0;
}

/* The bound instance the dispatch that is running was entered for (see the
 * in-process endpoint block below). */
static uint8_t nucleus_call_handle;
static uint8_t nucleus_call_object;

static int16_t ipc_call_endpoint(uint8_t number, struct kos_ipc_frame *frame);
static int16_t api_copy(uint8_t id, uint16_t offset, uint16_t driver_addr,
			uint16_t length, uint8_t dir);

/* Invoke one driver dispatch entry: open the owner's domain when crossing
 * into it, switch to the fixed driver stack at the outermost call, and run
 * the frame through the fixed primitive.  A nested call whose target is
 * already in the current domain takes the same-domain fast path and does
 * not touch the mapping. */
static int16_t invoke_endpoint(const struct ipc_endpoint *ep,
			       struct kos_ipc_frame *frame, uint8_t nested)
{
	uint8_t page = IPC_DRIVER_PAGE_BITS;
	int16_t result;

	/* An in-process endpoint: the dispatch entry is a bank-7 function
	 * address and the call runs in place, because the
	 * caller context already shows bank 7 - the syscall gate cleared
	 * the page field before dispatching.  A caller that does not is
	 * refused: switching the window here would touch the caller's
	 * own mapping. */
	if ((ep->flags & IPC_EP_NUCLEUS) != 0u) {
		int16_t (*dispatch)(uint8_t, uint8_t,
				    struct kos_ipc_frame *);

		if ((ipc_latch() & KOS_ROM_PAGE_MASK) !=
		    IPC_NUCLEUS_PAGE_BITS) {
			return -KOS_ENODEV;
		}
		dispatch = (int16_t (*)(uint8_t, uint8_t,
					struct kos_ipc_frame *))ep->dispatch;
		return dispatch(nucleus_call_handle, nucleus_call_object,
				frame);
	}
	if (!nested) {
		ipc_domain_enter(page);
		result = ipc_invoke(ep->dispatch, frame);
		ipc_domain_exit();
		return result;
	}
	if ((ipc_latch() & KOS_ROM_PAGE_MASK) == page) {
		return ipc_invoke_here(ep->dispatch, frame);
	}
	/* Cross-domain nested call: the nucleus re-selects the target domain
	 * around the call, but not through the save/restore primitive - the
	 * outermost call owns that single-depth state. */
	{
		uint8_t saved = (uint8_t)(ipc_latch() & KOS_ROM_PAGE_MASK);

		ipc_domain_set(page);
		result = ipc_invoke_here(ep->dispatch, frame);
		ipc_domain_set(saved);
	}
	return result;
}

/* Dispatch one call.  At the outermost level (call_depth == 0) the caller
 * may be an application or cold nucleus code whose frame is outside the
 * driver window, so it works on the fixed boundary buffer the callers
 * stage into.  Every nested call - a resident driver calling a resolved
 * peer - is near-direct (architecture 4.9): the caller's own frame is
 * passed by reference, the window already shows bank 6, and neither a
 * frame copy nor a bank switch happens. */
static int16_t ipc_call_endpoint(uint8_t handle, struct kos_ipc_frame *frame)
{
	struct ipc_endpoint *ep = endpoint_for(handle);
	uint8_t nested = call_depth;
	int16_t result;

	if (ep == (struct ipc_endpoint *)0) {
		return -KOS_EINVAL;
	}
	if (ep->busy && (ep->flags & KOS_IPC_EP_REENTRANT) == 0u) {
		return -KOS_EBUSY;
	}
	if (call_depth >= KOS_IPC_MAX_NESTING) {
		return -KOS_ENOSPC;
	}
	ep->busy = 1u;
	call_owner[call_depth] = ep;
	++call_depth;
	result = invoke_endpoint(ep, frame, nested);
	--call_depth;
	ep->busy = 0u;
	frame->status = result;
	return result;
}

/* --- in-process (nucleus) endpoints ------------------------------------- */

/* ipc_call_object() writes the nucleus identity statics immediately before
 * the dispatch and invoke_endpoint()'s nucleus branch reads them; every
 * other entry (a driver calling a dependency, nucleus code calling a
 * class) leaves them zero, because neither has a handle table entry. */

/* Install one in-process endpoint.  The nucleus calls this at
 * initialization with a number from the reserved range above the
 * host-assigned driver numbers. */
int8_t ipc_install_nucleus_endpoint(uint8_t number, uint8_t flags,
				    int16_t (*dispatch)(uint8_t handle,
							uint8_t object,
							struct kos_ipc_frame *frame))
{
	struct ipc_endpoint *ep;

	if (endpoint_count >= IPC_ENDPOINT_MAX || number == 0u ||
	    number < IPC_EP_NUCLEUS_BASE || dispatch == (void *)0 ||
	    endpoint_for(number) != (struct ipc_endpoint *)0 ||
	    (flags & (uint8_t)~KOS_IPC_EP_REENTRANT) != 0u) {
		return (int8_t)(-KOS_EINVAL);
	}
	ep = &endpoints[endpoint_count];
	ep->dispatch = (uint16_t)dispatch;
	ep->class = 0u;
	ep->instance = 0u;
	ep->flags = (uint8_t)(flags | IPC_EP_NUCLEUS);
	ep->version = 0u;
	ep->number = number;
	ep->busy = 0u;
	ep->bulk_base = 0u;
	ep->bulk_size = 0u;
	++endpoint_count;
	return 0;
}

/* Dispatch one call to a bound instance (the object family's SYS_CALL). */
int16_t ipc_call_object(uint8_t endpoint, uint8_t object, uint8_t handle,
			struct kos_ipc_frame *frame)
{
	int16_t result;

	nucleus_call_handle = handle;
	nucleus_call_object = object;
	result = ipc_call_endpoint(endpoint, frame);
	nucleus_call_handle = 0u;
	nucleus_call_object = 0u;
	return result;
}

/* Descriptor copy for a nucleus endpoint: the staged bytes in the common
 * staging block land in the caller buffer through the same checked copy
 * resolver a bank-6 driver uses. */
int16_t ipc_nucleus_copy_out(uint8_t desc, uint16_t offset, uint16_t length)
{
	return api_copy(desc, offset, (uint16_t)kos_user_copy_staging, length,
			KOS_IPC_DESC_OUT);
}

int16_t ipc_nucleus_copy_in(uint8_t desc, uint16_t offset, uint16_t length)
{
	return api_copy(desc, offset, (uint16_t)kos_user_copy_staging, length,
			KOS_IPC_DESC_IN);
}

/* --- nucleus-side calls -------------------------------------------------- */

/* The cold half's way into a driver: the same endpoint dispatch a syscall
 * takes, entered without a gate latch, so the frame carries no descriptor
 * (a bank-7 caller has no caller buffer to name).  The cold caller's frame
 * is staged through the fixed boundary buffer, the one frame a driver's
 * window always shows. */
int16_t ipc_nucleus_call(uint8_t handle, struct kos_ipc_frame *frame)
{
	int16_t result;

	if (frame == (struct kos_ipc_frame *)0) {
		return -KOS_EINVAL;
	}
	call_frame = *frame;
	result = ipc_call_endpoint(handle, &call_frame);
	*frame = call_frame;
	frame->status = result;
	return result;
}

/* Descriptor capture/release for nucleus callers with their own buffer
 * (the FS provider's enumeration forward, the cold loader's reads). */
int8_t ipc_desc_capture(uint8_t latch, uint16_t addr, uint16_t length,
			uint8_t dir)
{
	return desc_capture(latch, addr, length, dir);
}

void ipc_desc_release(void)
{
	desc_release_all();
}

/* Nucleus-side calls with a caller buffer at the current (outermost) call
 * level: the frame is staged in the fixed boundary buffer the driver
 * window always shows, the descriptor is captured for the duration of the
 * call, and both callers run on the driver stack. */
static int16_t ipc_nucleus_call_desc(uint8_t handle, struct kos_ipc_frame *frame,
				     uint8_t latch, uint16_t addr,
				     uint16_t length, uint8_t dir)
{
	int8_t id;
	int16_t result;

	if (frame == (struct kos_ipc_frame *)0) {
		return -KOS_EINVAL;
	}
	desc_release_all();
	id = desc_capture(latch, addr, length, dir);
	if (id < 0) {
		return -KOS_EINVAL;
	}
	call_frame = *frame;
	call_frame.desc = (uint8_t)id;
	result = ipc_call_endpoint(handle, &call_frame);
	*frame = call_frame;
	frame->status = result;
	desc_release_all();
	return result;
}

int16_t ipc_nucleus_call_object(uint8_t endpoint, uint8_t object,
				uint8_t handle, struct kos_ipc_frame *frame)
{
	int16_t result;

	if (frame == (struct kos_ipc_frame *)0) {
		return -KOS_EINVAL;
	}
	call_frame = *frame;
	result = ipc_call_object(endpoint, object, handle, &call_frame);
	*frame = call_frame;
	frame->status = result;
	return result;
}

int16_t ipc_nucleus_call_object_common(uint8_t endpoint, uint8_t object,
				       uint8_t handle,
				       struct kos_ipc_frame *frame,
				       uint16_t length, uint8_t dir)
{
	int8_t id;
	int16_t result;

	if (frame == (struct kos_ipc_frame *)0) {
		return -KOS_EINVAL;
	}
	desc_release_all();
	id = desc_capture(0u, (uint16_t)kos_user_copy_staging, length, dir);
	if (id < 0) {
		return -KOS_EINVAL;
	}
	call_frame = *frame;
	call_frame.desc = (uint8_t)id;
	result = ipc_call_object(endpoint, object, handle, &call_frame);
	*frame = call_frame;
	frame->status = result;
	desc_release_all();
	return result;
}

int16_t ipc_nucleus_call_gate(uint8_t handle, struct kos_ipc_frame *frame,
			      uint16_t addr, uint16_t length, uint8_t dir)
{
	return ipc_nucleus_call_desc(handle, frame, ipc_gate_latch(), addr,
				     length, dir);
}

int16_t ipc_nucleus_call_common(uint8_t handle, struct kos_ipc_frame *frame,
				uint16_t length, uint8_t dir)
{
	return ipc_nucleus_call_desc(handle, frame, 0u,
				     (uint16_t)kos_user_copy_staging, length,
				     dir);
}

/* --- the driver bootstrap interface ------------------------------------ */

/* The entries below are the ones `struct driver_api` (drivers/include/
 * driver.h) hands a resident driver.  They are global on purpose: a
 * driver calls each of them from its own window, where bank 7 shows only
 * the fixed core and the common region, and tools/mapcheck.py pins every
 * exported `api_*` entry to one of those two areas by its map symbol. */

static int16_t api_copy(uint8_t id, uint16_t offset, uint16_t driver_addr,
			uint16_t length, uint8_t dir)
{
	struct ipc_desc *desc;

	if (ipc_irq_active) {
		return -KOS_EBUSY;
	}
	desc = desc_lookup(id);
	if (desc == (struct ipc_desc *)0 || desc->dir != dir) {
		return -KOS_EINVAL;
	}
	return copy_move(desc, driver_addr, offset, length, dir);
}

int16_t api_call(uint8_t dependency_ref, uint8_t op,
			struct kos_ipc_frame *frame)
{
	if (ipc_irq_active) {
		return -KOS_EBUSY;
	}
	frame->op = op;
	return ipc_call_endpoint(dependency_ref, frame);
}

int16_t api_copy_in(uint8_t desc, uint16_t offset, uint16_t dst,
			   uint16_t length)
{
	return api_copy(desc, offset, dst, length, KOS_IPC_DESC_IN);
}

int16_t api_copy_out(uint8_t desc, uint16_t offset, uint16_t src,
			    uint16_t length)
{
	return api_copy(desc, offset, src, length, KOS_IPC_DESC_OUT);
}

int16_t api_move(uint16_t src, uint16_t dst, uint16_t length)
{
	if (ipc_irq_active) {
		return -KOS_EBUSY;
	}
	if (!in_driver_window(src, length) || !in_driver_window(dst, length)) {
		return -KOS_EINVAL;
	}
	ipc_copy_req[0] = src;
	ipc_copy_req[1] = dst;
	ipc_copy_req[2] = length;
	return (int16_t)ipc_copy_run();
}

/* Architecture 5.2: a driver's bulk data lives in bank 6's boot/bulk
 * tier (0000h-5FFFh), which the mode-11 window never shows; it is reached
 * only through a nucleus copy.  `base`/`size` were registered with the
 * calling driver's endpoints by the loader, so the copy is bound to the
 * driver that owns the current dispatch.  The transfer installs the
 * mode-10 mapping over bank 6 (P = 0, so 0000h-DFFFh all show bank 6) and
 * runs one LDIR from the bulk extent into the caller's own window buffer,
 * through the same common-resident primitive path B uses. */
int16_t api_bulk_read(uint16_t offset, uint16_t dst, uint16_t length)
{
	const struct ipc_endpoint *owner;
	uint16_t src;

	if (ipc_irq_active) {
		return -KOS_EBUSY;
	}
	if (call_depth == 0u) {
		return -KOS_EINVAL;
	}
	owner = call_owner[call_depth - 1u];
	if (owner->bulk_size == 0u || length == 0u ||
	    offset > owner->bulk_size ||
	    length > (uint16_t)(owner->bulk_size - offset) ||
	    !in_driver_window(dst, length)) {
		return -KOS_EINVAL;
	}
	src = (uint16_t)(owner->bulk_base + offset);
	if ((uint32_t)src + length > KOS_REV13_WINDOW_BASE) {
		return -KOS_EINVAL;
	}
	ipc_ap_req[0] = (uint8_t)(KOS_MODE_APPLICATION | IPC_DRIVER_BANK);
	ipc_ap_req[1] = (uint8_t)src;
	ipc_ap_req[2] = (uint8_t)(src >> 8);
	ipc_ap_req[3] = (uint8_t)dst;
	ipc_ap_req[4] = (uint8_t)(dst >> 8);
	ipc_ap_req[5] = (uint8_t)length;
	ipc_ap_req[6] = (uint8_t)(length >> 8);
	return (int16_t)ipc_ap_copy();
}

static __sfr __at KOS_SIO0B_CTRL ipc_sio_ctrl;
static __sfr __at KOS_SIO0B_DATA ipc_sio_data;

void api_console_putc(uint8_t byte)
{
	/* The early console path stays nucleus-owned and common-resident. */
	early_console_putc((char)byte);
}

int8_t api_console_poll(void)
{
	return (int8_t)((ipc_sio_ctrl & KOS_SIO_RR0_RX_AVAILABLE) != 0u
			? 1 : 0);
}

int8_t api_console_getc(void)
{
	return (int8_t)ipc_sio_data;
}

/* --- IRQ ownership: nucleus vectoring, driver handler ------------------- */

/* Fixed trampoline: entered from the IRQ executive on the ISR stack with
 * the interrupted latch saved in the IRQ gate's own slot.  It opens the
 * owner's window, runs the driver handler there (restricted API: no IPC),
 * and re-selects the nucleus page; the executive restores the interrupted
 * latch exactly and executes the final RETI.  The call's own saved latch
 * (ipc_domain_enter/exit) is never touched, so an IRQ during a driver
 * call cannot corrupt the call's epilogue. */
static void ipc_irq_trampoline(void *context)
{
	uint8_t source = (uint8_t)(uint16_t)context;

	if (source >= IRQ_SOURCE_COUNT || irq_handler_entry[source] == 0u) {
		return;
	}
	ipc_irq_active = 1u;
	ipc_domain_set(IPC_DRIVER_PAGE_BITS);
	((void (*)(void *))irq_handler_entry[source])(
		(void *)irq_handler_context[source]);
	ipc_domain_set(IPC_NUCLEUS_PAGE_BITS);
	ipc_irq_active = 0u;
}

int8_t api_irq_claim(uint8_t source, uint16_t handler,
			    uint16_t context)
{
	if (source >= IRQ_SOURCE_COUNT ||
	    handler < KOS_REV13_WINDOW_BASE ||
	    handler >= KOS_BANK7_RUNTIME_END) {
		return (int8_t)(-KOS_EINVAL);
	}
	if (irq_handler_entry[source] != 0u) {
		return (int8_t)(-KOS_EBUSY);
	}
	irq_handler_entry[source] = handler;
	irq_handler_context[source] = context;
	if (irq_claim((irq_source_t)source, ipc_irq_trampoline,
		      (void *)(uint16_t)source) != IRQ_OK) {
		irq_handler_entry[source] = 0u;
		return (int8_t)(-KOS_EBUSY);
	}
	return 0;
}

int8_t api_irq_release(uint8_t source)
{
	if (source >= IRQ_SOURCE_COUNT ||
	    irq_release((irq_source_t)source) != IRQ_OK) {
		return (int8_t)(-KOS_ENOENT);
	}
	irq_handler_entry[source] = 0u;
	irq_handler_context[source] = 0u;
	return 0;
}

/* Device events (architecture 4.10): the driver's ISR credits one event to
 * the subscription the nucleus handed it.  The bank the post interrupts
 * decides whether it counts as pending or as missed; see
 * kernel/event/event_common.asm kos_event_post_bank. */
void api_event_post(uint16_t sub)
{
	kos_event_post(sub, kos_event_post_bank(
				     (uint8_t)(ipc_irq_active ? 1u : 0u)));
}

static const struct driver_api ipc_api = {
	api_call,
	api_copy_in,
	api_copy_out,
	api_move,
	api_bulk_read,
	api_console_putc,
	api_console_poll,
	api_console_getc,
	api_irq_claim,
	api_irq_release,
	api_event_post,
};

/* The boot bundle a driver init entry receives in HL.  Both halves live in
 * the fixed core, which stays visible while the driver's window shows
 * bank 6; the loader reads the pointer here before it hands it to the
 * upcall shim, so it crosses no bank by itself. */
static struct driver_boot driver_boot_bundle = {
	&ipc_api,
	kos_driver_deps,
};

/* The boot bundle a driver init entry receives in HL; the loader installs
 * the resolved dependency numbers into kos_driver_deps before each init
 * (architecture 5.3). */
const struct driver_boot *driver_boot(void)
{
	return &driver_boot_bundle;
}

/* --- syscall entry ------------------------------------------------------- */

/* The syscall body resolved the bound handle to the endpoint and the
 * provider object first, so the block's `handle` rides along only for an
 * in-process provider that reads its instance from it. */
int16_t ipc_syscall_call_object(struct kos_ipc_call_args *args,
				uint8_t endpoint, uint8_t object)
{
	int8_t id = (int8_t)-1;
	int16_t result;

	desc_release_all();
	if (args->length != 0u) {
		id = desc_capture(ipc_gate_latch(), args->buffer,
				  args->length, args->direction);
		if (id < 0) {
			args->status = -KOS_EINVAL;
			return -KOS_EINVAL;
		}
	}
	call_frame = args->frame;
	call_frame.desc = (id >= 0) ? (uint8_t)id : KOS_IPC_DESC_NONE;
	result = ipc_call_object(endpoint, object, args->handle, &call_frame);
	args->frame = call_frame;
	args->status = result;
	desc_release_all();
	return result;
}
