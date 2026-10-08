/* ZephyrOS resident-service bootstrap interface (module-facing, M10).
 *
 * A resident service image receives exactly one table pointer: the init
 * entry named by its container is called with a `const struct
 * ipc_service_api *` in HL, and every nucleus primitive the service may
 * use is a function in that table (architecture 36.11).  There is no
 * kernel import table: a service relocates against its own image only.
 *
 * The table is versioned: `version` always carries
 * ZOS_IPC_SERVICE_API_VERSION; a service init may refuse a table that
 * does not (the format's service ABI version, include/zephyros/service.h,
 * tracks the same contract).
 *
 * Every entry, and why it is in the interface (36.11 asks for the list):
 *
 *   version         the table's version marker.
 *   register_ep     the one registration path: the resident loader
 *                   registers the endpoints the image's metadata declares
 *                   through it before any service code runs, and a
 *                   service that must add an endpoint the host cannot
 *                   name uses the same call.  Handles stay opaque and the
 *                   dispatch entry is validated against the window.
 *   lookup          dependency discovery by (class, instance) - never a
 *                   raw address or bank number; the SDK caches lookups,
 *                   the discovery query stays fixed.
 *   call            synchronous call/reply into a peer with the
 *                   nucleus-owned 32-byte frame; the only IPC model
 *                   (36.7).
 *   copy_in         caller descriptor -> service buffer, bounds checked
 *   copy_out        service buffer -> caller descriptor, bounds checked
 *                   on every use (36.8); a service never names a caller
 *                   address itself.
 *   move            same-domain copy between two service-domain addresses
 *                   (the staging step of chained providers), validated
 *                   against the service window.
 *   console_putc    the nucleus-owned early console, polled SIO0/B, which
 *   console_poll    is usable before the console service exists and stays
 *   console_getc    for service diagnostics and panic-time output (36.12).
 *   irq_claim       hand the nucleus one hardware source and an in-image
 *   irq_release     handler; the nucleus keeps the IM2 vectoring, the ISR
 *                   stack and the final RETI (36.10).
 *
 * There is no entry for paths, mounts, devices or file objects: those are
 * protocol payloads between services.
 *
 * Endpoint lifecycle:
 *   - register_ep(class, instance, dispatch, flags, version) returns a
 *     handle (1..N) or -ZOS_ENOSPC when the table is full;
 *   - lookup(class, instance) returns a peer's handle (or 0 when no
 *     endpoint matches);
 *   - call(handle, op, frame) dispatches synchronously into the peer and
 *     returns its status.  The frame is the nucleus-owned 32-byte copy;
 *     a service only ever reads and writes its own copy.
 *
 * Copies: a service moves data between its own domain and the caller's
 * buffer only through copy_in/copy_out with a nucleus-issued descriptor
 * id (the frame's `desc` field) and the caller's captured length.  Both
 * length and offset are bounds-checked on every use.  The service
 * address it names - its own I/O buffer, cache or staging block - belongs
 * in bank 6's 8000h-BFFFh, the one bank-6 range the mode-11 window and
 * the mode-10 aperture both show at the same address, so the nucleus can
 * move the run in one LDIR.  A buffer outside that range is still copied,
 * through the chunked path, and counted as slow (correction 01 to
 * architecture 36.8).  move() is the same-domain primitive for
 * service-to-service staging inside the resident bank; both addresses
 * must lie in the service window and are validated against it.
 *
 * IRQ ownership: a service claims a hardware source with
 * irq_claim(source, handler, context) and releases it with
 * irq_release(source).  The handler lives in the service image; the
 * nucleus keeps the IM2 vectoring, the ISR stack and the final RETI, and
 * enters the handler with the owner's window selected and the interrupted
 * latch saved in the IRQ gate's own slot (architecture 36.10).  A handler
 * runs under the restricted API: register_ep, lookup, call, copy_in,
 * copy_out and move are refused with -ZOS_EBUSY while an interrupt is
 * being delivered.  The console primitives are the nucleus-owned early
 * console (polled SIO0/B, common-resident) and stay callable.
 */

#ifndef ZEPHYROS_IPC_H
#define ZEPHYROS_IPC_H

#include "zos/ipc.h"
#include "zos/types.h"

/* The bootstrap interface's version (architecture 36.11).  A service init
 * may refuse a table that does not carry it; the format's service ABI
 * version (include/zephyros/service.h) tracks the same contract. */
#define ZOS_IPC_SERVICE_API_VERSION 1u

struct ipc_service_api {
	uint8_t version; /* ZOS_IPC_SERVICE_API_VERSION */

	/* Endpoint table (services, at boot only). */
	int8_t (*register_ep)(uint8_t class, uint8_t instance,
			      uint16_t entry, uint8_t flags,
			      uint8_t version);
	int8_t (*lookup)(uint8_t class, uint8_t instance);

	/* Synchronous call/reply with a nucleus-owned frame copy. */
	int16_t (*call)(uint8_t handle, uint8_t op,
			struct zos_ipc_frame *frame);

	/* Descriptor copies with bounds checks. */
	int16_t (*copy_in)(uint8_t desc, uint16_t offset, uint16_t dst,
			   uint16_t length);
	int16_t (*copy_out)(uint8_t desc, uint16_t offset, uint16_t src,
			    uint16_t length);
	int16_t (*move)(uint16_t src, uint16_t dst, uint16_t length);

	/* Nucleus-owned early console (used before the console service
	 * exists and for service diagnostics). */
	void (*console_putc)(uint8_t byte);
	int8_t (*console_poll)(void);
	int8_t (*console_getc)(void);

	/* IRQ ownership: claim a source with a handler in this image. */
	int8_t (*irq_claim)(uint8_t source, uint16_t handler,
			    uint16_t context);
	int8_t (*irq_release)(uint8_t source);
};

/* The container's init entry: void init(const struct ipc_service_api *). */
typedef void (*ipc_service_init_fn)(const struct ipc_service_api *api);

#endif /* ZEPHYROS_IPC_H */
