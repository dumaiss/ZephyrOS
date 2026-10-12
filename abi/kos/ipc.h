/* KonaOS native IPC syscall interface (public native ABI, M10 stage 3).
 *
 * The IPC substrate is the nucleus mechanism behind architecture sections
 * 36.7-36.9: drivers export endpoints, clients hold opaque handles, and a
 * call is one bounded 32-byte frame copied in at entry and back on reply.
 * Large payloads move through nucleus-issued buffer descriptors, never
 * through caller addresses: the ABI has no raw cross-domain pointer.
 *
 * Argument-block placement: both blocks live in the caller *window*,
 * 0100h-1FFFh (KOS_USER_BUFFER_BASE .. KOS_SYSCALL_WINDOW_END).  The
 * P-normalizing gate (kernel/syscall/entry.asm) leaves only that part of
 * the caller's bank visible while the nucleus runs, so a block anywhere
 * else is refused with carry set and A = KOS_EINVAL before the IPC layer
 * is entered.  A caller therefore marshals through a low-window scratch:
 * libkos exports `kos_scratch` (shared, 16 bytes, pinned by the program
 * manifest) and its own `kos_ipc_call_block` / `kos_ipc_raw_req` for the
 * two IPC numbers (docs/sdk.md).
 *
 * Native syscalls (docs/abi.md section 12):
 *
 *   RST 30h, A = KOS_SYSCALL_CALL, HL = &struct kos_ipc_call_args
 *       carry clear: the call was dispatched (or refused by the IPC
 *       layer); HL = the IPC status: 0 or positive from the driver,
 *       negative -KOS_E* for a refusal (unknown handle or descriptor,
 *       forged descriptor id, copy bounds, nesting depth, re-entrancy).
 *       The reply frame and the status are written back into the block.
 *       carry set:  A = KOS_EINVAL, the request never reached the IPC
 *       layer (block outside the caller window, bad frame length, bad
 *       reserved byte).
 *
 * Descriptor convention: one optional caller buffer per call, named by
 * `buffer`/`length`/`direction` in the call block.  The nucleus captures
 * it at entry from the caller's saved execution latch (bank and page) as
 * a small id, hands the id to the driver in the frame's `desc` field,
 * and refuses (KOS_EINVAL) any address outside the caller buffer window
 * 0100h-DFFFh (the caller's own bank) plus the common window
 * E000h-FFFFh, which every mapping shows at the same address, or any
 * length above KOS_IPC_DESC_MAX.  Drivers reach the buffer only through
 * the nucleus copies, with bounds checked on every use:
 *
 *   copy_in (id, offset, driver_buffer, length)   caller -> driver
 *   copy_out(id, offset, driver_buffer, length)   driver -> caller
 *
 * `offset + length` may never exceed the captured descriptor length.
 * A caller address in 8000h-BFFFh with a nonzero saved page names the
 * far bank shown by the mode-10 aperture (architecture 36.17); the copy
 * resolvers use the resolved bank, not the primary one.
 *
 * Copy paths (correction 01 to architecture 36.8).  Which mapping moves
 * the bytes is picked per request, and all three paths are correct for
 * every accepted descriptor; only the first two are fast:
 *
 *   A  the caller buffer is in 0000h-1FFFh (the caller window mode 11
 *      shows) or in the common window: one LDIR in the driver mapping.
 *   B  the caller buffer is in the caller's bank clear of 8000h-BFFFh
 *      and the driver buffer lies in bank 6's 8000h-BFFFh: one LDIR
 *      with mode 10 selected over the caller's bank and page 1, so the
 *      aperture shows bank 6 at the address the driver named.  The
 *      primitive restores the latch exactly and is interruptible; page 1
 *      is nucleus-internal and never appears in a caller's latch.
 *   C  everything else, including a driver buffer outside
 *      8000h-BFFFh: chunked through the nucleus's application-bank copy
 *      gate, and counted as slow.
 *
 * A driver buffer that is the source or target of copy_in/copy_out
 * therefore belongs in bank 6's 8000h-BFFFh: that is the one bank-6
 * range the mode-11 window and the mode-10 aperture both show at the
 * same address.  A driver that places one elsewhere still works, on
 * path C.
 *
 * The frame is fixed at 32 bytes and is the only request/reply envelope:
 * 8-byte header plus 24 payload bytes.  Operation numbers, flags and
 * payload encodings belong to the versioned driver protocols, never to
 * this header.
 */

#ifndef KOS_IPC_H
#define KOS_IPC_H

#include "kos/types.h"

/* Native syscall numbers.  Package 42b retired the stage-3
 * KOS_SYSCALL_IPC_LOOKUP (06h): a client reaches a provider through a
 * bound namespace handle (the ABI's kos/object.h), never by class/instance. */
#define KOS_SYSCALL_CALL   0x07u

/* The request/reply frame: fixed size, copied at every crossing. */
#define KOS_IPC_FRAME_SIZE   32u
#define KOS_IPC_PAYLOAD_SIZE 24u

/* Nesting limit: calls deep (application -> VFS -> IOC-FAT -> IOC and the
 * loader's upcall chain).  A deeper call is refused with KOS_ENOSPC. */
#define KOS_IPC_MAX_NESTING 4u

/* Descriptor ids are small integers assigned by the nucleus; the frame
 * carries KOS_IPC_DESC_NONE when the call named no buffer. */
#define KOS_IPC_DESC_NONE 0xFFu

/* Largest caller buffer the nucleus captures (one bounded cluster). */
#define KOS_IPC_DESC_MAX 4096u

/* Buffer descriptor directions. */
#define KOS_IPC_DESC_IN  0u /* caller writes, driver reads (copy_in)  */
#define KOS_IPC_DESC_OUT 1u /* driver writes, caller reads (copy_out) */

/* Endpoint registration flags (driver bootstrap interface). */
#define KOS_IPC_EP_REENTRANT 0x01u /* a call into an active endpoint runs */

struct kos_ipc_frame {
	uint8_t op;                        /* protocol operation number  */
	uint8_t flags;                     /* protocol flags             */
	uint8_t desc;                      /* nucleus descriptor id      */
	uint8_t arg0;                      /* protocol word 0            */
	uint16_t arg1;                     /* protocol word 1            */
	uint8_t payload[KOS_IPC_PAYLOAD_SIZE]; /* inline payload words   */
	int16_t status;                    /* call status, filled by the nucleus */
};

/* 07h CALL {handle, op, flags, frame_length, buffer, length, direction,
 * reserved, frame, status}.  `frame_length` is the protocol frame size
 * (1..32); `buffer`/`length`/`direction` name the one optional caller
 * buffer (length 0 means no descriptor and `buffer` is ignored), and
 * `reserved` must be zero.  The driver's reply frame and status land
 * back in the block. */
struct kos_ipc_call_args {
	uint8_t handle;
	uint8_t op;
	uint8_t flags;
	uint8_t frame_length;
	uint16_t buffer;
	uint16_t length;
	uint8_t direction;
	uint8_t reserved;
	struct kos_ipc_frame frame;
	int16_t status;
};

#if defined(__SDCC)
_Static_assert(sizeof(struct kos_ipc_frame) == KOS_IPC_FRAME_SIZE,
	       "the IPC frame is fixed at 32 bytes");
_Static_assert(__builtin_offsetof(struct kos_ipc_frame, payload) == 6,
	       "IPC frame payload must follow the header words");
_Static_assert(__builtin_offsetof(struct kos_ipc_frame, status) == 30,
	       "IPC frame status must be the last field");
_Static_assert(sizeof(struct kos_ipc_call_args) == 44,
	       "IPC call argument block must be 44 packed bytes");
_Static_assert(__builtin_offsetof(struct kos_ipc_call_args, frame) == 10,
	       "IPC call frame offset");
_Static_assert(__builtin_offsetof(struct kos_ipc_call_args, status) == 42,
	       "IPC call status offset");
#endif

#endif /* KOS_IPC_H */
