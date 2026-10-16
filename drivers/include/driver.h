/* KonaOS resident-driver bootstrap interface (module-facing).
 *
 * A resident driver image receives exactly one pointer: the init entry
 * named by its container is called with a `const struct
 * driver_boot *` in HL, naming the nucleus table the driver may use
 * and the host-resolved dependency array (architecture 5.3).  There is
 * no kernel import table: a driver is linked against its own image and
 * its host-resolved endpoints only.
 *
 * There is no version field: the host packer links the driver against
 * this one table, and the packer validates on the host.
 *
 * Every entry, and why it is in the interface (5.3 asks for the list):
 *
 *   call            synchronous call/reply into a peer named by a
 *                   host-resolved dependency number or an application
 *                   handle; the only IPC model (4.9).  A call from one
 *                   resident driver to another is near-direct: the frame
 *                   is passed by reference and the target's window is
 *                   already open, so no frame copy and no bank switch
 *                   happen.
 *   copy_in         caller descriptor -> driver buffer, bounds checked
 *   copy_out        driver buffer -> caller descriptor, bounds checked
 *                   on every use (4.9); a driver never names a caller
 *                   address itself.
 *   move            same-domain copy between two driver-domain addresses
 *                   (the staging step of chained providers), validated
 *                   against the driver window.
 *   bulk_read       the calling driver's own bulk data -> a buffer in its
 *                   own window, bounds checked against the registered
 *                   bulk extent.  Architecture 5.2 keeps bulk data
 *                   (the V9958 font) in bank 6's boot/bulk tier
 *                   (0000h-5FFFh), which the mode-11 driver window never
 *                   shows; a run-time path that needs it - the V9958
 *                   display reset re-uploads the font atlases - reads it
 *                   through this nucleus copy.  The loader registers the
 *                   extent with each image it places, and only the
 *                   driver that owns the endpoint being dispatched may
 *                   read it.
 *   console_putc    the nucleus-owned early console, polled SIO0/B, which
 *   console_poll    is usable before the console driver exists and stays
 *   console_getc    for driver diagnostics and panic-time output (3.2).
 *   irq_claim       hand the nucleus one hardware source and an in-image
 *   irq_release     handler; the nucleus keeps the IM2 vectoring, the ISR
 *                   stack and the final RETI (4.5).
 *   event_post      post one device event for the subscription id the
 *                   nucleus issued the driver through KOS_OP_SUBSCRIBE;
 *                   the nucleus delivers it to the subscriber at a safe
 *                   point, once that process runs again (4.10).
 *
 * There is no entry for paths, mounts, devices or file objects: those are
 * protocol payloads between drivers.
 *
 * Endpoint lifecycle (architecture 5.3): the host assembly resolves
 * every endpoint number and every inter-driver dependency; the loader
 * installs the host-built tables before any driver code runs, and the
 * driver's init receives its resolved dependency numbers in
 * `struct driver_boot.deps`, one byte per declared dependency in
 * manifest order (0 = an optional dependency this build does not
 * export).  A driver never looks another up at run time:
 *   - call(dependency_ref, op, frame) dispatches synchronously into the peer
 *     named by a resolved dependency number (or an application handle)
 *     and returns its status.  The frame is the caller's 32-byte
 *     envelope; the outermost application call copies it once at the
 *     gate boundary, and a driver-to-driver call passes its own frame by
 *     reference.
 *
 * Copies: a driver moves data between its own domain and the caller's
 * buffer only through copy_in/copy_out with a nucleus-issued descriptor
 * id (the frame's `desc` field) and the caller's captured length.  Both
 * length and offset are bounds-checked on every use.  The driver
 * address it names - its own I/O buffer, cache or staging block - belongs
 * in bank 6's 8000h-BFFFh, the one bank-6 range the mode-11 window and
 * the mode-10 aperture both show at the same address, so the nucleus can
 * move the run in one LDIR.  A buffer outside that range is still copied,
 * through the chunked path, and counted as slow (architecture 4.9).  move() is the same-domain primitive for
 * driver-to-driver staging inside the resident bank; both addresses
 * must lie in the driver window and are validated against it.
 *
 * IRQ ownership: a driver claims a hardware source with
 * irq_claim(source, handler, context) and releases it with
 * irq_release(source).  The handler lives in the driver image; the
 * nucleus keeps the IM2 vectoring, the ISR stack and the final RETI, and
 * enters the handler with the owner's window selected and the interrupted
 * latch saved in the IRQ gate's own slot (architecture 4.5).  A handler
 * runs under the restricted API: call, copy_in, copy_out and move are
 * refused with -KOS_EBUSY while an interrupt is being delivered.  The
 * console primitives are the nucleus-owned early console (polled SIO0/B,
 * common-resident) and stay callable.
 */

#ifndef KONAOS_IPC_H
#define KONAOS_IPC_H

#include "kos/ipc.h"
#include "kos/types.h"

/* Bound of the resolved dependency array the loader installs
 * (MOD_MAX_DEPENDENCIES, formats/include/mod.h). */
#define KOS_IPC_DRIVER_MAX_DEPS 8u

/* The logical IRQ sources a resident driver may claim with `irq_claim`
 * (the executive's irq_source_t numbering, kernel/include/irq_types.h).
 * A source id is not a vector byte: the platform wiring is kernel data. */
#define KOS_IRQ_CTC0   0u
#define KOS_IRQ_CTC1   1u
#define KOS_IRQ_CTC2   2u
#define KOS_IRQ_CTC3   3u
#define KOS_IRQ_SIO0   4u
#define KOS_IRQ_SIO1   5u
#define KOS_IRQ_VDP_FF 6u

struct driver_api {
	/* Synchronous call/reply with a nucleus-owned frame copy. */
	int16_t (*call)(uint8_t dependency_ref, uint8_t op,
			struct kos_ipc_frame *frame);

	/* Descriptor copies with bounds checks. */
	int16_t (*copy_in)(uint8_t desc, uint16_t offset, uint16_t dst,
			   uint16_t length);
	int16_t (*copy_out)(uint8_t desc, uint16_t offset, uint16_t src,
			    uint16_t length);
	int16_t (*move)(uint16_t src, uint16_t dst, uint16_t length);

	/* The calling driver's own bulk data (registered by the loader):
	 * copies `length` bytes at `offset` into `dst`, a buffer in this
	 * driver's window, and returns the length.  Refused (-KOS_EINVAL)
	 * when the extent, the offset/length pair or the destination is out
	 * of bounds, -KOS_EBUSY while an interrupt is delivered. */
	int16_t (*bulk_read)(uint16_t offset, uint16_t dst, uint16_t length);

	/* Nucleus-owned early console (used before the console driver
	 * exists and for driver diagnostics). */
	void (*console_putc)(uint8_t byte);
	int8_t (*console_poll)(void);
	int8_t (*console_getc)(void);

	/* IRQ ownership: claim a source with a handler in this image.
	 * One handler per source; a driver that owns more than one channel
	 * (the timer) claims each. */
	int8_t (*irq_claim)(uint8_t source, uint16_t handler,
			    uint16_t context);
	int8_t (*irq_release)(uint8_t source);

	/* Device events (architecture 4.10): post one event for the
	 * subscription id SYS_NOTIFY handed the driver.  ISR-safe, and meant
	 * for the handler: the nucleus counts it as pending while the
	 * subscribing process is the one the interrupted context runs for,
	 * and drops it into that subscriber's `missed` otherwise (its child
	 * running, another process being served).  Nothing is queued and
	 * nothing is delivered here: the nucleus delivers on the
	 * subscriber's next return to its own code.  An endpoint that
	 * declared no events (MOD_EP_EVENTS) is never given a subscription
	 * id, and a post with an id it did not issue is ignored. */
	void (*event_post)(uint16_t sub);
};

/* The boot bundle the loader hands a driver's init: the nucleus table and
 * the resolved dependency numbers, one byte per declared dependency in
 * manifest order (0 = an optional dependency this build does not export).
 * Both live in the fixed core, which stays visible while the driver's
 * window shows bank 6. */
struct driver_boot {
	const struct driver_api *api;
	const uint8_t *deps;
};

/* The container's init entry: `uint8_t init(const struct driver_boot
 * *)`, returning 0 when the driver is initialized.  A non-zero status is
 * an explicit boot failure: the loader refuses the image and the nucleus
 * stops on the early console (5.3). */
typedef uint8_t (*driver_init_fn)(const struct driver_boot *boot);

/* The drivers' block copy (drivers/runtime/sdcc_indirect_calls.asm, linked
 * into every driver image): one LDIR, 21 T-states a byte.  A data path
 * copies with this, never with a C byte loop, which SDCC compiles to
 * several hundred T-states a byte.  `count` 0 copies nothing. */
void kos_block_copy(void *dst, const void *src, uint16_t count);

#endif /* KONAOS_IPC_H */
