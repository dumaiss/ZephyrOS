/* ZephyrOS kernel-private IPC interface (kernel/ipc).
 *
 * The substrate itself is fixed-core code: the endpoint table, the call
 * machinery, the descriptor table, the copy resolvers and the IRQ
 * trampoline all execute, and are touched, while the window shows bank 6
 * (architecture 36.5).  This header declares only what the cold side
 * (the syscall bodies) and the fixed assembly primitives expose.
 *
 * Cold callers run with the P-normalizing syscall gate's saved latch in
 * zos_gate_saved_latch (read through ipc_gate_latch, kernel/ipc/ipc_asm.asm);
 * every descriptor is captured from that latch, so the caller's bank and
 * page survive the gate's normalization.
 */

#ifndef ZEPHYROS_KERNEL_IPC_IPC_H
#define ZEPHYROS_KERNEL_IPC_IPC_H

#include "zos/ipc.h"
#include "zos/object.h"
#include "zos/types.h"

#include "driver.h"

/* The syscall gate's saved caller latch (bank, mode and page), read from
 * common state (kernel/ipc/ipc_asm.asm).  The copy resolver and the
 * buffer-identity rule (kernel/syscall.c `zos_syscall_buffer_bank`) both
 * resolve caller addresses against it. */
uint8_t ipc_gate_latch(void);

/* One host-built endpoint record (architecture 36.11), read from a
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

/* Install one endpoint record.  Returns 0, or -ZOS_EINVAL/-ZOS_ENOSPC.
 * Replaces runtime registration: the loader installs the host-built table
 * before any driver code runs. */
int8_t ipc_install_endpoint(const struct ipc_endpoint_record *record);

/* The fixed-core dependency array the boot bundle names: the loader
 * installs the resolved endpoint numbers of the image whose init it is
 * about to run, one byte per declared dependency in manifest order, and
 * zeros the unused tail (architecture 36.11).  Fixed data, so the driver's
 * window always shows it. */
extern uint8_t zos_driver_deps[ZOS_IPC_DRIVER_MAX_DEPS];

/* The boot bundle the loader hands a driver's init entry in HL. */
const struct driver_boot *driver_boot(void);

/* --- in-process (nucleus) endpoints ------------------------------------
 *
 * Architecture 36.13: a namespace provider the nucleus serves itself -
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
							struct zos_ipc_frame *frame));

/* Dispatch one call to a bound instance: `endpoint` is the endpoint the
 * handle resolved to, `object` the provider identity from the publication
 * and `handle` the caller's handle number (an in-process provider reads
 * its instance from it). */
int16_t ipc_call_object(uint8_t endpoint, uint8_t object, uint8_t handle,
			struct zos_ipc_frame *frame);

/* SYS_CALL body: validate and dispatch the caller's block.  The block is
 * in the caller window (visible while the gate dispatches with P = 0);
 * the reply frame, status and descriptor capture land back in it, and the
 * caller's bound handle resolved to `endpoint`/`object` by the syscall
 * body first (kernel/vfs/router.c).  The return value is the IPC status
 * (the
 * syscall publishes it in HL with carry clear). */
int16_t ipc_syscall_call_object(struct zos_ipc_call_args *args,
				uint8_t endpoint, uint8_t object);

/* Descriptor capture and release for a nucleus caller that has a caller
 * buffer of its own to hand a driver (the FS provider's enumeration
 * forward, the cold loader's image stream).  `latch` is the buffer's
 * execution latch: the syscall gate's saved latch for a caller buffer,
 * or 0 when the address is in the common window, which every mapping
 * shows.  A returned id is valid for the call the nucleus makes next and
 * must be released afterwards. */
int8_t ipc_desc_capture(uint8_t latch, uint16_t addr, uint16_t length,
			uint8_t dir);
void ipc_desc_release(void);

/* --- nucleus-side calls -------------------------------------------------- */

/* Synchronous call into a driver from nucleus code (bank 7, no process
 * and no syscall gate): the boot console probe.  No descriptor is issued -
 * only the syscall gate's latch names a caller buffer - so a caller that
 * has to move bytes uses ipc_nucleus_call_common() or the staged form
 * plus the common copy gate (architecture 36.8).  `frame` is the caller's
 * own 32-byte copy; the reply lands back in it. */
int16_t ipc_nucleus_call(uint8_t handle, struct zos_ipc_frame *frame);

/* Nucleus call with one caller buffer: the same dispatch, with a
 * descriptor captured for `addr`/`length` from the syscall gate's saved
 * latch (the FS provider's enumeration forward). */
int16_t ipc_nucleus_call_gate(uint8_t handle, struct zos_ipc_frame *frame,
			      uint16_t addr, uint16_t length, uint8_t dir);

/* Nucleus call whose buffer is the common staging block, `length` bytes
 * (the cold loader's FILE reads, `length` at most
 * ZOS_SYSCALL_STAGE_SIZE): path A in every mapping, so no bank-7 scratch
 * is ever named to a driver. */
int16_t ipc_nucleus_call_common(uint8_t handle, struct zos_ipc_frame *frame,
				uint16_t length, uint8_t dir);

/* The same two forms for a bound instance: the object/handle pair is set
 * for an in-process provider that reads its instance from it (the cold
 * loader's reads on a ROMFS FILE handle). */
int16_t ipc_nucleus_call_object(uint8_t endpoint, uint8_t object,
				uint8_t handle, struct zos_ipc_frame *frame);
int16_t ipc_nucleus_call_object_common(uint8_t endpoint, uint8_t object,
				       uint8_t handle,
				       struct zos_ipc_frame *frame,
				       uint16_t length, uint8_t dir);

/* Descriptor copy for an in-process provider: `length` requested bytes
 * move between the caller buffer of descriptor `desc` (at `offset`) and
 * the common staging block, through the same checked copy resolver a
 * bank-6 driver uses. */
int16_t ipc_nucleus_copy_out(uint8_t desc, uint16_t offset, uint16_t length);
int16_t ipc_nucleus_copy_in(uint8_t desc, uint16_t offset, uint16_t length);

#endif /* ZEPHYROS_KERNEL_IPC_IPC_H */
