/* ZephyrOS resident-driver bootstrap interface (module-facing, M10).
 *
 * A resident driver image receives exactly one pointer: the init entry
 * named by its container is called with a `const struct
 * ipc_service_boot *` in HL, naming the nucleus table the driver may use
 * and the host-resolved dependency array (architecture 36.11).  There is
 * no kernel import table: a driver is linked against its own image and
 * its host-resolved endpoints only.
 *
 * There is no version field: the host packer links the driver against
 * this one table (work package 42c removed the runtime ABI/version
 * checks; the packer validates on the host).
 *
 * Every entry, and why it is in the interface (36.11 asks for the list):
 *
 *   call            synchronous call/reply into a peer named by a
 *                   host-resolved dependency number or an application
 *                   handle; the only IPC model (36.7).  A call from one
 *                   resident driver to another is near-direct: the frame
 *                   is passed by reference and the target's window is
 *                   already open, so no frame copy and no bank switch
 *                   happen.
 *   copy_in         caller descriptor -> service buffer, bounds checked
 *   copy_out        service buffer -> caller descriptor, bounds checked
 *                   on every use (36.8); a service never names a caller
 *                   address itself.
 *   move            same-domain copy between two service-domain addresses
 *                   (the staging step of chained providers), validated
 *                   against the service window.
 *   bulk_read       the calling service's own bulk data -> a buffer in its
 *                   own window, bounds checked against the registered
 *                   bulk extent.  Architecture 36.6 keeps bulk data
 *                   (the V9958 font) in bank 6's boot/bulk tier
 *                   (0000h-5FFFh), which the mode-11 service window never
 *                   shows; a run-time path that needs it - the V9958
 *                   display reset re-uploads the font atlases - reads it
 *                   through this nucleus copy.  The loader registers the
 *                   extent with each image it places, and only the
 *                   service that owns the endpoint being dispatched may
 *                   read it.
 *   console_putc    the nucleus-owned early console, polled SIO0/B, which
 *   console_poll    is usable before the console service exists and stays
 *   console_getc    for service diagnostics and panic-time output (36.12).
 *   irq_claim       hand the nucleus one hardware source and an in-image
 *   irq_release     handler; the nucleus keeps the IM2 vectoring, the ISR
 *                   stack and the final RETI (36.10).
 *
 * There is no entry for paths, mounts, devices or file objects: those are
 * protocol payloads between drivers.
 *
 * Endpoint lifecycle (architecture 36.11): the host assembly resolves
 * every endpoint number and every inter-driver dependency; the loader
 * installs the host-built tables before any driver code runs, and the
 * driver's init receives its resolved dependency numbers in
 * `struct ipc_service_boot.deps`, one byte per declared dependency in
 * manifest order (0 = an optional dependency this build does not
 * export).  A driver never looks another up at run time:
 *   - call(handle, op, frame) dispatches synchronously into the peer
 *     named by a resolved dependency number (or an application handle)
 *     and returns its status.  The frame is the caller's 32-byte
 *     envelope; the outermost application call copies it once at the
 *     gate boundary, and a driver-to-driver call passes its own frame by
 *     reference.
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
 * runs under the restricted API: call, copy_in, copy_out and move are
 * refused with -ZOS_EBUSY while an interrupt is being delivered.  The
 * console primitives are the nucleus-owned early console (polled SIO0/B,
 * common-resident) and stay callable.
 */

#ifndef ZEPHYROS_IPC_H
#define ZEPHYROS_IPC_H

#include "zos/ipc.h"
#include "zos/types.h"

/* Bound of the resolved dependency array the loader installs
 * (ZMOD_MAX_DEPENDENCIES, include/zephyros/service.h). */
#define ZOS_IPC_SERVICE_MAX_DEPS 8u

struct ipc_service_api {
	/* Synchronous call/reply with a nucleus-owned frame copy. */
	int16_t (*call)(uint8_t handle, uint8_t op,
			struct zos_ipc_frame *frame);

	/* Descriptor copies with bounds checks. */
	int16_t (*copy_in)(uint8_t desc, uint16_t offset, uint16_t dst,
			   uint16_t length);
	int16_t (*copy_out)(uint8_t desc, uint16_t offset, uint16_t src,
			    uint16_t length);
	int16_t (*move)(uint16_t src, uint16_t dst, uint16_t length);

	/* The calling service's own bulk data (registered by the loader):
	 * copies `length` bytes at `offset` into `dst`, a buffer in this
	 * service's window, and returns the length.  Refused (-ZOS_EINVAL)
	 * when the extent, the offset/length pair or the destination is out
	 * of bounds, -ZOS_EBUSY while an interrupt is delivered. */
	int16_t (*bulk_read)(uint16_t offset, uint16_t dst, uint16_t length);

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

/* The boot bundle the loader hands a driver's init: the nucleus table and
 * the resolved dependency numbers, one byte per declared dependency in
 * manifest order (0 = an optional dependency this build does not export).
 * Both live in the fixed core, which stays visible while the driver's
 * window shows bank 6. */
struct ipc_service_boot {
	const struct ipc_service_api *api;
	const uint8_t *deps;
};

/* The container's init entry: `uint8_t init(const struct ipc_service_boot
 * *)`, returning 0 when the driver is initialized.  A non-zero status is
 * an explicit boot failure: the loader refuses the image and the nucleus
 * stops on the early console (36.11, work package 42e). */
typedef uint8_t (*ipc_service_init_fn)(const struct ipc_service_boot *boot);

#endif /* ZEPHYROS_IPC_H */
