/* ZephyrOS resident-service spike: the nucleus-side IPC substrate
 * (work package 35, test only; tests/target/rsvc/).
 *
 * Linked only into an evaluation image (ZOS_RSVC_SPIKE=1) and compiled
 * with the fixed-core areas, so the whole substrate stays visible in
 * 2000h-5FFFh while the resident service domain (bank 6) is mapped into
 * the mode-11 window (6000h-DFFFh at the revision-13 split).
 *
 * What this file implements:
 *   - the endpoint table: owner class/instance, domain, dispatch entry,
 *     re-entrancy flag and busy flag, filled by service registration;
 *   - service_lookup(class, instance) -> handle and call(handle, op,
 *     frame) with a bounded 32-byte frame copied at every crossing;
 *   - nucleus-issued buffer descriptors: captured and validated at call
 *     entry (execution bank, caller address, length, direction), handed
 *     to services as small ids and usable only through copy_in/copy_out;
 *   - the bank-6 resident load of the synthetic services: ZMOD
 *     containers relocated through the existing relocate.c code with the
 *     domain mapped, then registered through the API table;
 *   - the IRQ trampoline: the nucleus keeps vectoring, the ISR stack and
 *     the final RETI; the service handler runs in the service domain on
 *     the nucleus ISR stack with no IPC available to it;
 *   - the evaluation-only syscall bodies the client program drives.
 *
 * Everything here is a prototype for measurement.  There is no
 * revocation, no asynchronous queue, no generic marshaling: the frame is
 * fixed-size and per-protocol operation numbers live with the services.
 */

#include "zos/syscall.h"
#include "zos/types.h"
#include "zephyros/errno.h"
#include "zephyros/irq.h"
#include "zephyros/module.h"
#include "zephyros/kernel.h"

#include "zephyr80/hardware.h"

#include "kernel/syscall/diag.h"
#include "kernel/irq/irq_private.h"

#include "ctc_test.h"
#include "rsvc_test.h"
#include "rsvc_config.h"

/* Fixed-region primitives (tests/target/rsvc/rsvc_asm.asm). */
extern uint8_t rsvc_latch(void);
extern uint8_t rsvc_latch_saved(void);
extern void rsvc_domain_enter(uint8_t page_bits);
extern void rsvc_domain_set(uint8_t page_bits);
extern void rsvc_domain_exit(void);
extern uint16_t rsvc_copy_req[3];
extern uint16_t rsvc_copy_run(void);
extern int16_t rsvc_invoke(uint16_t entry, struct rsvc_frame *frame);
extern int16_t rsvc_invoke_here(uint16_t entry, struct rsvc_frame *frame);
extern uint16_t rsvc_saved_sp;
extern uint8_t rsvc_service_stack[];

/* Kernel stack addresses (fixed-region and common objects the kernel
 * defines in its own assembler namespace). */
extern uint16_t rsvc_gate_stack_top(void);
extern uint8_t *rsvc_isr_stack(void);
extern uint16_t rsvc_isr_stack_size(void);

/* Resident service containers, emitted by tools/zmodpack.py emit-asm. */
extern const uint8_t rsvc_echo_zmod[];
extern const uint16_t rsvc_echo_zmod_size;
extern const uint8_t rsvc_con_zmod[];
extern const uint16_t rsvc_con_zmod_size;
extern const uint8_t rsvc_fat_zmod[];
extern const uint16_t rsvc_fat_zmod_size;
extern const uint8_t rsvc_ioc_zmod[];
extern const uint16_t rsvc_ioc_zmod_size;
extern const uint8_t rsvc_vfs_zmod[];
extern const uint16_t rsvc_vfs_zmod_size;
extern const uint8_t rsvc_irqx_zmod[];
extern const uint16_t rsvc_irqx_zmod_size;

/* --- endpoint and descriptor tables ------------------------------------ */

struct rsvc_endpoint {
	uint16_t dispatch; /* service dispatch entry in the resident domain */
	uint8_t class;
	uint8_t instance;
	uint8_t flags;
	uint8_t version;
	uint8_t registered;
	uint8_t busy;
};

static struct rsvc_endpoint endpoints[RSVC_MAX_ENDPOINTS];
static uint8_t endpoint_count;

#define RSVC_MAX_DESCS 4u

struct rsvc_desc {
	uint8_t active;
	uint8_t dir;     /* 0 = in (caller writes), 1 = out (caller reads) */
	uint8_t depth;   /* call level the descriptor belongs to           */
	uint8_t bank;    /* execution bank captured at call entry           */
	uint16_t addr;
	uint16_t length;
};

static struct rsvc_desc descs[RSVC_MAX_DESCS];

/* The nucleus-owned frame copies, one per call level.  frames[0] is the
 * client's frame; a call at level d works on frames[d]. */
static struct rsvc_frame frames[ZOS_RSVC_MAX_NESTING + 1u];

static uint8_t call_depth;
static uint8_t force_full_path;
static uint8_t rsvc_ready;
static uint8_t rsvc_irq_baseline;

/* --- evidence block (fixed region, read by the replay and the client) -- */

struct rsvc_evidence rsvc_evidence;

/* --- helpers ------------------------------------------------------------ */

static void evidence_u16_inc(uint16_t *field)
{
	*field = (uint16_t)(*field + 1u);
}

static void evidence_note(uint8_t counter, uint16_t value)
{
	switch (counter) {
	case RSVC_NOTE_SERVICE_CALLS:
		evidence_u16_inc(&rsvc_evidence.service_calls);
		break;
	case RSVC_NOTE_CHAIN_WORDS:
		rsvc_evidence.chain_words =
			(uint16_t)(rsvc_evidence.chain_words + value);
		break;
	case RSVC_NOTE_EXIT_NOTIFY:
		evidence_u16_inc(&rsvc_evidence.exit_notify);
		break;
	case RSVC_NOTE_STREAM_BYTES:
		rsvc_evidence.stream_bytes =
			(uint16_t)(rsvc_evidence.stream_bytes + value);
		break;
	case RSVC_NOTE_SERVICE_MARK:
		rsvc_evidence.reserved0 = (uint8_t)value;
		break;
	default:
		break;
	}
}

/* True when `addr` (length `length`) lies inside the resident domain the
 * current latch maps: the whole mode-11 window.  A service can name only
 * its own domain this way, so a caller address is refused. */
static uint8_t domain_ok(uint16_t addr, uint16_t length, uint8_t page_bits)
{
	uint16_t base = ZOS_RSVC_DOMAIN_BASE;
	uint16_t size = ZOS_RSVC_DOMAIN_SIZE;
	uint16_t end;

	if (length == 0u || length > (uint16_t)(0xFFFFu - addr)) {
		return 0u;
	}
	(void)page_bits;
	end = (uint16_t)(base + size);
	return (uint8_t)(addr >= base && (uint16_t)(addr + length) <= end);
}

/* The latch's bank field during a call is the caller's execution bank:
 * the syscall gate preserves it, and every domain mapping keeps it. */
static uint8_t caller_bank(void)
{
	return (uint8_t)(rsvc_latch() & 0x07u);
}

/* --- descriptor capture and the copy primitives ------------------------- */

static void desc_release_all(void)
{
	uint8_t index;

	for (index = 0u; index < RSVC_MAX_DESCS; ++index) {
		descs[index].active = 0u;
	}
}

static int8_t desc_capture(uint16_t addr, uint16_t length, uint8_t dir)
{
	uint8_t index;

	if (length == 0u) {
		return (int8_t)RSVC_DESC_NONE;
	}
	if (dir > 1u || length > ZOS_RSVC_DESC_MAX ||
	    addr < ZOS_SYSCALL_WINDOW_BASE ||
	    (uint16_t)(addr + length) > ZOS_SYSCALL_WINDOW_END) {
		evidence_u16_inc(&rsvc_evidence.desc_refused);
		return (int8_t)RSVC_DESC_NONE;
	}
	for (index = 0u; index < RSVC_MAX_DESCS; ++index) {
		if (!descs[index].active) {
			descs[index].active = 1u;
			descs[index].dir = dir;
			descs[index].depth = call_depth;
			descs[index].bank = caller_bank();
			descs[index].addr = addr;
			descs[index].length = length;
			rsvc_evidence.desc_captured =
				(uint8_t)(rsvc_evidence.desc_captured + 1u);
			return (int8_t)index;
		}
	}
	evidence_u16_inc(&rsvc_evidence.desc_refused);
	return (int8_t)RSVC_DESC_NONE;
}

static struct rsvc_desc *desc_lookup(uint8_t id)
{
	if (id >= RSVC_MAX_DESCS) {
		evidence_u16_inc(&rsvc_evidence.desc_forged);
		return (struct rsvc_desc *)0;
	}
	if (!descs[id].active || descs[id].depth >= call_depth) {
		evidence_u16_inc(&rsvc_evidence.desc_forged);
		return (struct rsvc_desc *)0;
	}
	return &descs[id];
}

static int16_t copy_checked(uint8_t id, uint16_t service_addr,
			    uint16_t length, uint8_t dir)
{
	struct rsvc_desc *desc = desc_lookup(id);
	uint8_t page = (uint8_t)(rsvc_latch() & 0xE0u);

	if (desc == (struct rsvc_desc *)0 || desc->dir != dir) {
		evidence_u16_inc(&rsvc_evidence.desc_forged);
		return -ZOS_EINVAL;
	}
	if (length == 0u || length > desc->length) {
		evidence_u16_inc(&rsvc_evidence.desc_forged);
		return -ZOS_EINVAL;
	}
	if (!domain_ok(service_addr, length, page)) {
		evidence_u16_inc(&rsvc_evidence.desc_forged);
		return -ZOS_EINVAL;
	}
	if (dir == 0u) {
		rsvc_copy_req[0] = desc->addr;
		rsvc_copy_req[1] = service_addr;
	} else {
		rsvc_copy_req[0] = service_addr;
		rsvc_copy_req[1] = desc->addr;
	}
	rsvc_copy_req[2] = length;
	return (int16_t)rsvc_copy_run();
}

static int16_t api_copy_in(uint8_t desc, uint16_t dst, uint16_t length)
{
	return copy_checked(desc, dst, length, 0u);
}

static int16_t api_copy_out(uint8_t desc, uint16_t src, uint16_t length)
{
	return copy_checked(desc, src, length, 1u);
}

static int16_t api_move(uint16_t src, uint16_t dst, uint16_t length)
{
	uint8_t page = (uint8_t)(rsvc_latch() & 0xE0u);

	if (!domain_ok(src, length, page) || !domain_ok(dst, length, page)) {
		evidence_u16_inc(&rsvc_evidence.desc_forged);
		return -ZOS_EINVAL;
	}
	rsvc_copy_req[0] = src;
	rsvc_copy_req[1] = dst;
	rsvc_copy_req[2] = length;
	return (int16_t)rsvc_copy_run();
}

static int16_t api_desc_info(uint8_t id, uint8_t *direction)
{
	struct rsvc_desc *desc;

	if (id >= RSVC_MAX_DESCS || !descs[id].active) {
		evidence_u16_inc(&rsvc_evidence.desc_forged);
		return -ZOS_EINVAL;
	}
	desc = &descs[id];
	if (direction != (uint8_t *)0) {
		*direction = desc->dir;
	}
	return (int16_t)desc->length;
}

/* --- endpoint registration, lookup and dispatch -------------------------- */

static struct rsvc_endpoint *endpoint_for(uint8_t handle)
{
	if (handle == 0u || handle > endpoint_count) {
		return (struct rsvc_endpoint *)0;
	}
	if (!endpoints[handle - 1u].registered) {
		return (struct rsvc_endpoint *)0;
	}
	return &endpoints[handle - 1u];
}

static int8_t rsvc_lookup(uint8_t class, uint8_t instance, uint8_t *version)
{
	uint8_t index;

	for (index = 0u; index < endpoint_count; ++index) {
		if (endpoints[index].registered &&
		    endpoints[index].class == class &&
		    endpoints[index].instance == instance) {
			if (version != (uint8_t *)0) {
				*version = endpoints[index].version;
			}
			return (int8_t)(index + 1u);
		}
	}
	return 0;
}

static int8_t api_register_ep(uint8_t class, uint8_t instance,
			      uint16_t entry, uint8_t flags, uint8_t version)
{
	struct rsvc_endpoint *ep;

	if (endpoint_count >= RSVC_MAX_ENDPOINTS || entry == 0u) {
		return (int8_t)(-ZOS_ENOSPC);
	}
	ep = &endpoints[endpoint_count];
	ep->dispatch = entry;
	ep->class = class;
	ep->instance = instance;
	ep->flags = (uint8_t)(flags | RSVC_EP_SAMEDOMAIN);
	ep->version = version;
	ep->registered = 1u;
	ep->busy = 0u;
	++endpoint_count;
	rsvc_evidence.endpoints = endpoint_count;
	return (int8_t)endpoint_count;
}

/* Invoke one service dispatch entry: open the domain when crossing into
 * it, switch to the service stack at the outermost call, and run the
 * bounded frame through the fixed-region primitive. */
static int16_t invoke_endpoint(const struct rsvc_endpoint *ep,
			       struct rsvc_frame *frame, uint8_t nested)
{
	uint8_t page = ZOS_RSVC_PAGE_BITS;
	int16_t result;

	if (!nested) {
		rsvc_domain_enter(page);
		result = rsvc_invoke(ep->dispatch, frame);
		if (rsvc_evidence.gate_sp_min == 0u ||
		    rsvc_saved_sp < rsvc_evidence.gate_sp_min) {
			rsvc_evidence.gate_sp_min = rsvc_saved_sp;
		}
		rsvc_domain_exit();
		rsvc_evidence.latch_ok =
			(uint8_t)(rsvc_latch() == rsvc_latch_saved());
		return result;
	}
	if (!force_full_path && (ep->flags & RSVC_EP_SAMEDOMAIN) != 0u &&
	    (rsvc_latch() & 0xE0u) == page) {
		evidence_u16_inc(&rsvc_evidence.fast_path_calls);
		return rsvc_invoke_here(ep->dispatch, frame);
	}
	/* Full path from inside the domain: the nucleus re-selects the
	 * target domain around the call even when caller and target share
	 * it, so the same-domain fast path can be compared against it.
	 * The nested call must not use the save/restore primitive: the
	 * outermost call owns that single-depth state. */
	{
		uint8_t saved = rsvc_latch();

		rsvc_domain_set(page);
		result = rsvc_invoke_here(ep->dispatch, frame);
		rsvc_domain_set((uint8_t)(saved & 0xE0u));
	}
	return result;
}

static int16_t rsvc_call_endpoint(uint8_t handle, struct rsvc_frame *frame)
{
	struct rsvc_endpoint *ep = endpoint_for(handle);
	struct rsvc_frame *target;
	uint8_t nested = call_depth;
	int16_t result;

	if (ep == (struct rsvc_endpoint *)0) {
		return -ZOS_EINVAL;
	}
	if (ep->busy && (ep->flags & RSVC_EP_REENTRANT) == 0u) {
		evidence_u16_inc(&rsvc_evidence.reentrant_refused);
		return -ZOS_EBUSY;
	}
	if (call_depth >= ZOS_RSVC_MAX_NESTING) {
		evidence_u16_inc(&rsvc_evidence.depth_refused);
		return -ZOS_ENOSPC;
	}
	target = &frames[call_depth + 1u];
	*target = *frame;
	ep->busy = 1u;
	++call_depth;
	if (call_depth > rsvc_evidence.nested_max) {
		rsvc_evidence.nested_max = call_depth;
	}
	result = invoke_endpoint(ep, target, nested);
	--call_depth;
	ep->busy = 0u;
	*frame = *target;
	frame->result = result;
	evidence_u16_inc(&rsvc_evidence.calls);
	return result;
}

/* --- API table ----------------------------------------------------------- */

static int16_t api_call(uint8_t handle, uint8_t op, struct rsvc_frame *frame)
{
	frame->op = op;
	return rsvc_call_endpoint(handle, frame);
}

static __sfr __at ZOS_SIO0B_CTRL rsvc_sio_ctrl;
static __sfr __at ZOS_SIO0B_DATA rsvc_sio_data;

static void api_console_putc(uint8_t byte)
{
	/* The early console path stays nucleus-owned: the spike console
	 * service forwards each byte through this primitive, the same
	 * polled SIO0/B write the nucleus uses with no driver resident. */
	uint16_t timeout = ZOS_SIO_TX_TIMEOUT;

	while (timeout--) {
		if ((rsvc_sio_ctrl & ZOS_SIO_TX_READY) == ZOS_SIO_TX_READY) {
			rsvc_sio_data = byte;
			rsvc_evidence.con_bytes =
				(uint16_t)(rsvc_evidence.con_bytes + 1u);
			return;
		}
	}
}

static int8_t api_console_poll(void)
{
	return (int8_t)((rsvc_sio_ctrl & ZOS_SIO_RR0_RX_AVAILABLE) != 0u
			? 1 : 0);
}

static int8_t api_console_getc(void)
{
	return (int8_t)rsvc_sio_data;
}

/* --- IRQ ownership: nucleus vectoring, service handler ------------------- */

static uint16_t rsvc_service_irq_handler;
static uint16_t rsvc_service_irq_context;
static uint8_t rsvc_irq_page_bits;

/* Fixed-region trampoline: entered from the IRQ executive on the ISR
 * stack with the interrupted latch saved by the entry code.  It opens
 * the service domain, runs the service handler there (restricted API:
 * the handler must not issue IPC), and returns; the executive restores
 * the interrupted latch and executes the final RETI. */
static void rsvc_irq_trampoline(void *context)
{
	uint8_t before = rsvc_latch();

	(void)context;
	/* The IRQ entry owns the interrupted latch (irq_saved_latch) and
	 * restores it exactly on the way out, so the trampoline must not
	 * touch the outermost call's single-depth save/restore state: the
	 * IRQ interrupts a service call, and rsvc_domain_enter would
	 * overwrite the latch that call must restore.  Select the page
	 * only and leave the mapping to the executive. */
	rsvc_domain_set(rsvc_irq_page_bits);
	evidence_u16_inc(&rsvc_evidence.irq_count);
	rsvc_evidence.irq_in_domain =
		(uint8_t)((before & 0xE0u) == rsvc_irq_page_bits ? 1u : 0u);
	rsvc_evidence.irq_baseline = rsvc_irq_baseline;
	if (!rsvc_irq_baseline && rsvc_service_irq_handler != 0u) {
		((void (*)(void *))rsvc_service_irq_handler)(
			(void *)rsvc_service_irq_context);
	}
}

static int8_t api_irq_claim(uint8_t source, uint16_t handler,
			    uint16_t context)
{
	if (handler == 0u) {
		return (int8_t)(-ZOS_EINVAL);
	}
	rsvc_service_irq_handler = handler;
	rsvc_service_irq_context = context;
	rsvc_irq_page_bits = ZOS_RSVC_PAGE_BITS;
	if (irq_claim((irq_source_t)source, rsvc_irq_trampoline,
		      (void *)0) != IRQ_OK) {
		return (int8_t)(-ZOS_EBUSY);
	}
	return 0;
}

static int8_t api_lookup(uint8_t class, uint8_t instance)
{
	return rsvc_lookup(class, instance, (uint8_t *)0);
}

static const struct rsvc_api rsvc_api = {
	api_register_ep,
	api_lookup,
	api_call,
	api_copy_in,
	api_copy_out,
	api_move,
	api_desc_info,
	api_console_putc,
	api_console_poll,
	api_console_getc,
	api_irq_claim,
	evidence_note,
};

/* --- bank-6 resident loading --------------------------------------------- */

struct rsvc_image {
	const uint8_t *blob;
	const uint16_t *size;
};

/* Load order is the dependency order: a service resolves its peers by
 * class in its own init, so the provider must be registered first. */
static const struct rsvc_image rsvc_images[] = {
	{ rsvc_ioc_zmod, &rsvc_ioc_zmod_size },
	{ rsvc_fat_zmod, &rsvc_fat_zmod_size },
	{ rsvc_vfs_zmod, &rsvc_vfs_zmod_size },
	{ rsvc_echo_zmod, &rsvc_echo_zmod_size },
	{ rsvc_con_zmod, &rsvc_con_zmod_size },
	{ rsvc_irqx_zmod, &rsvc_irqx_zmod_size },
};

#define RSVC_IMAGE_COUNT (sizeof(rsvc_images) / sizeof(rsvc_images[0]))

static int rsvc_blob_read(const void *ctx, uint16_t offset, uint8_t *dst,
			  uint16_t length)
{
	const uint8_t *src = (const uint8_t *)ctx + offset;
	uint16_t index;

	for (index = 0u; index < length; ++index) {
		dst[index] = src[index];
	}
	return (int)length;
}

static int8_t rsvc_load_one(const struct rsvc_image *image,
			    uint16_t *cursor)
{
	struct zmod_source source;
	struct zmod_container container;
	typedef void (*init_fn)(const struct rsvc_api *);
	uint16_t used = (uint16_t)(ZOS_RSVC_DOMAIN_BASE + *cursor);
	init_fn init;
	int result;

	source.read = rsvc_blob_read;
	source.ctx = image->blob;
	source.size = *image->size;
	if (zmod_validate_source(&source, &container) != ZMOD_OK) {
		return (int8_t)(-ZOS_EIO);
	}
	if ((uint32_t)used + container.image_size + container.bss_size >
	    (uint32_t)ZOS_RSVC_DOMAIN_BASE + ZOS_RSVC_DOMAIN_SIZE) {
		return (int8_t)(-ZOS_ENOSPC);
	}
	rsvc_domain_enter(ZOS_RSVC_PAGE_BITS);
	result = zmod_copy_image((uint8_t *)used, &container);
	if (result == ZMOD_OK) {
		zmod_zero_bss((uint8_t *)used, &container);
		result = zmod_apply_relocations((uint8_t *)used, &container);
	}
	if (result == ZMOD_OK) {
		init = (init_fn)(used + container.init_offset);
		init(&rsvc_api);
	}
	rsvc_domain_exit();
	if (result != ZMOD_OK) {
		return (int8_t)(-ZOS_EIO);
	}
	*cursor = (uint16_t)(*cursor + container.image_size +
			     container.bss_size);
	*cursor = (uint16_t)((*cursor + 15u) & 0xFFF0u);
	return 0;
}

static int8_t rsvc_load_images(void)
{
	uint16_t cursor = 0u;
	uint8_t index;
	int8_t result;

	for (index = 0u; index < RSVC_IMAGE_COUNT; ++index) {
		result = rsvc_load_one(&rsvc_images[index], &cursor);
		if (result != 0) {
			return result;
		}
	}
	return 0;
}

/* --- stacks -------------------------------------------------------------- */

static uint16_t stack_high_water(const uint8_t *base, uint16_t size)
{
	uint16_t index;

	for (index = 0u; index < size; ++index) {
		if (base[index] != 0xAAu) {
			return (uint16_t)(size - index);
		}
	}
	return 0u;
}

static void rsvc_paint_stacks(void)
{
	uint16_t index;

	for (index = 0u; index < ZOS_RSVC_SERVICE_STACK_SIZE; ++index) {
		rsvc_service_stack[index] = 0xAAu;
	}
	{
		uint8_t *isr = rsvc_isr_stack();

		for (index = 0u; index < rsvc_isr_stack_size(); ++index) {
			isr[index] = 0xAAu;
		}
	}
	rsvc_evidence.gate_sp_min = 0u;
	rsvc_evidence.stack_service = 0u;
	rsvc_evidence.stack_gate = 0u;
	rsvc_evidence.stack_isr = 0u;
}

static uint16_t rsvc_scan_stacks(void)
{
	rsvc_evidence.stack_service = stack_high_water(
		rsvc_service_stack, ZOS_RSVC_SERVICE_STACK_SIZE);
	rsvc_evidence.stack_isr = stack_high_water(rsvc_isr_stack(),
						   rsvc_isr_stack_size());
	if (rsvc_evidence.gate_sp_min != 0u) {
		rsvc_evidence.stack_gate = (uint16_t)(
			rsvc_gate_stack_top() - rsvc_evidence.gate_sp_min);
	}
	return (uint16_t)(rsvc_evidence.stack_service +
			  rsvc_evidence.stack_gate +
			  rsvc_evidence.stack_isr);
}

/* --- evaluation init and the syscall bodies ------------------------------ */

static void rsvc_init(void)
{
	rsvc_paint_stacks();
	if (rsvc_load_images() != 0) {
		return;
	}
	rsvc_ready = 1u;
}

static void copy_to_caller(uint16_t dst, const uint8_t *src, uint16_t length)
{
	uint8_t *out = (uint8_t *)dst;
	uint16_t index;

	for (index = 0u; index < length; ++index) {
		out[index] = src[index];
	}
}

static void rsvc_evidence_read(uint16_t block)
{
	uint16_t dst;

	if (!zos_syscall_block_ok(block, sizeof(uint16_t))) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	dst = (uint16_t)(((const uint8_t *)block)[0] |
			 ((uint16_t)((const uint8_t *)block)[1] << 8));
	if (dst < ZOS_SYSCALL_WINDOW_BASE ||
	    (uint16_t)(dst + ZOS_RSVC_EVIDENCE_SIZE) >
	    ZOS_SYSCALL_WINDOW_END) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	copy_to_caller(dst, (const uint8_t *)&rsvc_evidence,
		       ZOS_RSVC_EVIDENCE_SIZE);
	zos_syscall_success(ZOS_RSVC_EVIDENCE_SIZE);
}

static void rsvc_call_body(uint16_t block, uint8_t full)
{
	struct rsvc_call_req *req = (struct rsvc_call_req *)block;
	int8_t desc = (int8_t)RSVC_DESC_NONE;
	int16_t result;

	if (!zos_syscall_block_ok(block, sizeof(struct rsvc_call_req))) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	if (req->frame_len == 0u || req->frame_len > RSVC_FRAME_MAX) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	desc_release_all();
	force_full_path = full;
	if (req->buf_len != 0u) {
		desc = desc_capture(req->buf_addr, req->buf_len, req->buf_dir);
		if (desc == (int8_t)RSVC_DESC_NONE) {
			force_full_path = 0u;
			zos_syscall_error(ZOS_EINVAL);
			return;
		}
	}
	frames[0] = req->frame;
	frames[0].desc = (uint8_t)desc;
	result = rsvc_call_endpoint(req->handle, &frames[0]);
	req->frame = frames[0];
	req->result = result;
	force_full_path = 0u;
	desc_release_all();
	zos_syscall_success((uint16_t)result);
}

static void rsvc_irq_body(uint16_t block)
{
	struct rsvc_irq_req *req = (struct rsvc_irq_req *)block;
	struct rsvc_frame frame;
	int8_t handle = rsvc_lookup(RSVC_CLASS_IRQX, 0u, (uint8_t *)0);

	if (!zos_syscall_block_ok(block, sizeof(struct rsvc_irq_req))) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	if (handle <= 0) {
		zos_syscall_error(ZOS_ENOSYS);
		return;
	}
	frame.op = RSVC_OP_IRQX_SPIN;
	frame.flags = RSVC_FRAME_SPIN;
	frame.desc = RSVC_DESC_NONE;
	frame.arg0 = 0u;
	frame.arg1 = req->iterations;
	frame.result = 0;
	rsvc_irq_baseline = req->baseline;
	zos_ctc0_test_start();
	/* Run the spin inside the service domain with interrupts on: the
	 * IRQ fires while a service call is in progress.  The gate restores
	 * the caller's interrupt state on return regardless. */
	(void)irq_arch_lock();
	irq_arch_unlock(1u);
	frame.result = rsvc_call_endpoint((uint8_t)handle, &frame);
	(void)irq_arch_lock();
	zos_ctc0_test_stop();
	rsvc_irq_baseline = 0u;
	zos_syscall_success((uint16_t)frame.result);
}

static void rsvc_upcall_body(uint16_t block)
{
	struct rsvc_upcall_req *req = (struct rsvc_upcall_req *)block;
	struct rsvc_frame frame;
	int8_t handle = rsvc_lookup(RSVC_CLASS_VFS, 0u, (uint8_t *)0);

	if (!zos_syscall_block_ok(block, sizeof(struct rsvc_upcall_req))) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	if (handle <= 0) {
		zos_syscall_error(ZOS_ENOSYS);
		return;
	}
	frame.flags = 0u;
	frame.desc = RSVC_DESC_NONE;
	frame.result = 0;
	frame.arg1 = req->argument;
	if (req->kind == RSVC_UPCALL_EXIT) {
		frame.op = RSVC_OP_VFS_ON_EXIT;
		frame.arg0 = (uint8_t)req->argument;
		req->result = rsvc_call_endpoint((uint8_t)handle, &frame);
		evidence_u16_inc(&rsvc_evidence.upcall_exit);
	} else if (req->kind == RSVC_UPCALL_EXEC) {
		frame.op = RSVC_OP_VFS_EXEC;
		frame.arg0 = RSVC_OBJECT_HANDLE;
		req->result = rsvc_call_endpoint((uint8_t)handle, &frame);
		evidence_u16_inc(&rsvc_evidence.upcall_exec);
	} else {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	zos_syscall_success((uint16_t)req->result);
}

static void rsvc_stacks_body(uint16_t block)
{
	uint8_t words[6];
	uint16_t dst;

	if (!zos_syscall_block_ok(block, sizeof(uint16_t))) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	dst = (uint16_t)(((const uint8_t *)block)[0] |
			 ((uint16_t)((const uint8_t *)block)[1] << 8));
	if (dst < ZOS_SYSCALL_WINDOW_BASE ||
	    (uint16_t)(dst + 6u) > ZOS_SYSCALL_WINDOW_END) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	(void)rsvc_scan_stacks();
	words[0] = (uint8_t)rsvc_evidence.stack_service;
	words[1] = (uint8_t)(rsvc_evidence.stack_service >> 8);
	words[2] = (uint8_t)rsvc_evidence.stack_gate;
	words[3] = (uint8_t)(rsvc_evidence.stack_gate >> 8);
	words[4] = (uint8_t)rsvc_evidence.stack_isr;
	words[5] = (uint8_t)(rsvc_evidence.stack_isr >> 8);
	copy_to_caller(dst, words, 6u);
	zos_syscall_success(6u);
}

static void rsvc_lookup_body(uint16_t block)
{
	struct rsvc_lookup_req *req = (struct rsvc_lookup_req *)block;
	int8_t handle;

	if (!zos_syscall_block_ok(block, sizeof(struct rsvc_lookup_req))) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	req->handle = 0u;
	req->version = 0u;
	handle = rsvc_lookup(req->class, req->instance, &req->version);
	if (handle > 0) {
		req->handle = (uint8_t)handle;
	}
	zos_syscall_success((uint16_t)handle);
}

void zos_rsvc_dispatch(uint8_t number, uint8_t bank, uint16_t block)
{
	(void)bank;
	if (!rsvc_ready) {
		rsvc_init();
		if (!rsvc_ready) {
			zos_syscall_error(ZOS_EAGAIN);
			return;
		}
	}
	switch (number) {
	case ZOS_RSVC_SYSCALL_CALL:
		rsvc_call_body(block, 0u);
		break;
	case ZOS_RSVC_SYSCALL_CALL_FULL:
		rsvc_call_body(block, 1u);
		break;
	case ZOS_RSVC_SYSCALL_EVIDENCE:
		rsvc_evidence_read(block);
		break;
	case ZOS_RSVC_SYSCALL_IRQ:
		rsvc_irq_body(block);
		break;
	case ZOS_RSVC_SYSCALL_PAINT:
		rsvc_paint_stacks();
		zos_syscall_success(0u);
		break;
	case ZOS_RSVC_SYSCALL_STACKS:
		rsvc_stacks_body(block);
		break;
	case ZOS_RSVC_SYSCALL_UPCALL:
		rsvc_upcall_body(block);
		break;
	case ZOS_RSVC_SYSCALL_LOOKUP:
		rsvc_lookup_body(block);
		break;
	default:
		zos_syscall_error(ZOS_ENOSYS);
		break;
	}
}
