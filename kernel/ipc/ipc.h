/* KonaOS kernel-private IPC interface (kernel/ipc).
 *
 * The substrate itself is fixed-core code: the endpoint table, the call
 * machinery, the descriptor table, the copy resolvers and the IRQ
 * trampoline all execute, and are touched, while the window shows bank 6
 * (architecture 4.2).  This header declares only what the cold side
 * (the syscall bodies) and the fixed assembly primitives expose.
 *
 * Cold callers run with the P-normalizing syscall gate's saved latch in
 * kos_gate_saved_latch (read through ipc_gate_latch, kernel/ipc/ipc_asm.asm);
 * every descriptor is captured from that latch, so the caller's bank and
 * page survive the gate's normalization.
 */

#ifndef KONAOS_KERNEL_IPC_IPC_H
#define KONAOS_KERNEL_IPC_IPC_H

#include "kos/ipc.h"
#include "kos/object.h"
#include "kos/types.h"

#include "driver.h"

#if defined(KOS_BOARD_MK2)
/* Mk2: the caller's mapping value (its page-0 page), saved by the syscall
 * gate in common state (kernel/memory/mk2/banking.asm). */
extern uint8_t kos_gate_saved_latch;
#define ipc_gate_latch() (kos_gate_saved_latch)
#else
/* The syscall gate's saved caller latch (bank, mode and page), read from
 * common state (kernel/ipc/ipc_asm.asm).  The copy resolver and the
 * buffer-identity rule (kernel/syscall.c `kos_syscall_buffer_bank`) both
 * resolve caller addresses against it. */
uint8_t ipc_gate_latch(void);
#endif

/* One host-built endpoint record (architecture 5.3), read from a
 * resident driver container by the loader and installed verbatim.  The
 * nucleus validates the entry against the driver window and the number
 * against its table. */
struct ipc_endpoint_record {
	uint8_t class;
	uint8_t instance;
	uint8_t version;
	uint8_t flags;
	uint8_t number;    /* host-assigned endpoint number */
	uint16_t entry;    /* absolute bank-6 dispatch address */
	uint16_t bulk_base;
	uint16_t bulk_size;
};

/* Install one endpoint record.  Returns 0, or -KOS_EINVAL/-KOS_ENOSPC.
 * Replaces runtime registration: the loader installs the host-built table
 * before any driver code runs. */
int8_t ipc_install_endpoint(const struct ipc_endpoint_record *record);

/* The flags byte the endpoint `number` was installed with, or 0 when no
 * such endpoint exists.  The VFS reads the release declaration from it
 * before it tells an endpoint that one of its handles went away. */
uint8_t ipc_endpoint_flags(uint8_t number);

/* The fixed-core dependency array the boot bundle names: the loader
 * installs the resolved endpoint numbers of the image whose init it is
 * about to run, one byte per declared dependency in manifest order, and
 * zeros the unused tail (architecture 5.3).  Fixed data, so the driver's
 * window always shows it. */
extern uint8_t kos_driver_deps[KOS_IPC_DRIVER_MAX_DEPS];

/* The boot bundle the loader hands a driver's init entry in HL. */
const struct driver_boot *driver_boot(void);

#if defined(KOS_BOARD_MK2)
/* The driver domain's pages by domain logical page (entry 7 the common
 * page): the loader allocates them as segments land. */
extern uint8_t kos_driver_domain[];

/* Run a driver's boot entry (`boot` 1: masked, the domain's page 0 at
 * system entry 0 too) or its init entry (HL = `arg`) with the domain
 * mapped; returns the entry's 8-bit result. */
uint8_t ipc_driver_run(uint16_t entry, uint16_t arg, uint8_t boot);
#endif

/* --- in-process (nucleus) endpoints ------------------------------------
 *
 * Architecture 4.10: a namespace provider the nucleus serves itself -
 * the ROMFS FILE provider - exports an endpoint like any other; its
 * dispatch entry is a bank-7 function address and the call runs in place.
 * The flag bit is nucleus-internal and never comes from a host record. */
#define IPC_EP_NUCLEUS 0x80u

/* The reserved nucleus endpoint range: above every host-assigned driver
 * number (the loader numbers the driver records from 1, bounded by
 * IPC_ENDPOINT_MAX). */
#define IPC_EP_NUCLEUS_BASE 0x80u

int8_t ipc_install_nucleus_endpoint(uint8_t number, uint8_t flags,
				    int16_t (*dispatch)(uint8_t handle,
							uint8_t object,
							struct kos_ipc_frame *frame));

/* Dispatch one call to a bound instance: `endpoint` is the endpoint the
 * handle resolved to, `object` the provider identity from the publication
 * and `handle` the caller's handle number (an in-process provider reads
 * its instance from it). */
int16_t ipc_call_object(uint8_t endpoint, uint8_t object, uint8_t handle,
			struct kos_ipc_frame *frame);

/* SYS_CALL body: validate and dispatch the caller's block.  The block is
 * in the caller window (visible while the gate dispatches with P = 0);
 * the reply frame, status and descriptor capture land back in it, and the
 * caller's bound handle resolved to `endpoint`/`object` by the syscall
 * body first (its handle entry, kernel/vfs/handles.c).  The return
 * value is the IPC status (the syscall publishes it in HL with carry
 * clear). */
int16_t ipc_syscall_call_object(struct kos_ipc_call_args *args,
				uint8_t endpoint, uint8_t object);

/* --- nucleus-side calls -------------------------------------------------- */

/* Synchronous call into a driver from nucleus code (bank 7, no process
 * and no syscall gate): the boot console probe.  No descriptor is issued -
 * only the syscall gate's latch names a caller buffer - so a caller that
 * has to move bytes uses ipc_nucleus_call_common() or the staged form
 * plus the common copy gate (architecture 4.9).  `frame` is the caller's
 * own 32-byte copy; the reply lands back in it. */
int16_t ipc_nucleus_call(uint8_t handle, struct kos_ipc_frame *frame);

/* Nucleus call with one caller buffer: the same dispatch, with a
 * descriptor captured for `addr`/`length` from the syscall gate's saved
 * latch (the FS provider's enumeration forward). */
int16_t ipc_nucleus_call_gate(uint8_t handle, struct kos_ipc_frame *frame,
			      uint16_t addr, uint16_t length, uint8_t dir);

#if defined(KOS_BOARD_MK2)
/* Nucleus call on endpoint `endpoint` for a bound instance (the
 * object/handle pair is set for an in-process provider that reads its
 * instance from it; 0/0 for a driver): with a nonzero `length`, one OUT
 * descriptor of `length` bytes (at most KOS_IPC_DESC_MAX) at `addr`
 * through the page list `map` (in the fixed region or common, which the
 * driver's call keeps mapped), so the provider's bytes land straight in
 * those pages (the cold loader's FILE reads into a child's pages or its
 * own data); length 0 is the descriptor-less form. */
int16_t ipc_nucleus_call_pages(uint8_t endpoint, uint8_t object,
			       uint8_t handle, struct kos_ipc_frame *frame,
			       const uint8_t *map, uint16_t addr,
			       uint16_t length);
#else
/* Nucleus call whose buffer is the common staging block, `length` bytes
 * (the cold loader's FILE reads, `length` at most
 * KOS_SYSCALL_STAGE_SIZE): path A in every mapping, so no bank-7 scratch
 * is ever named to a driver. */
int16_t ipc_nucleus_call_common(uint8_t handle, struct kos_ipc_frame *frame,
				uint16_t length, uint8_t dir);

/* The same two forms for a bound instance: the object/handle pair is set
 * for an in-process provider that reads its instance from it (the cold
 * loader's reads on a ROMFS FILE handle). */
int16_t ipc_nucleus_call_object(uint8_t endpoint, uint8_t object,
				uint8_t handle, struct kos_ipc_frame *frame);
int16_t ipc_nucleus_call_object_common(uint8_t endpoint, uint8_t object,
				       uint8_t handle,
				       struct kos_ipc_frame *frame,
				       uint16_t length, uint8_t dir);
#endif

/* Descriptor copy for an in-process provider: `length` requested bytes
 * move between the caller buffer of descriptor `desc` (at `offset`) and
 * the common staging block, through the same checked copy resolver a
 * bank-6 driver uses. */
int16_t ipc_nucleus_copy_out(uint8_t desc, uint16_t offset, uint16_t length);
int16_t ipc_nucleus_copy_in(uint8_t desc, uint16_t offset, uint16_t length);

#endif /* KONAOS_KERNEL_IPC_IPC_H */
