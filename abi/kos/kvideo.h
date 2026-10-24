/* KonaOS VIDEO driver direct-graphics ABI (public native ABI).
 *
 * `/dev/video` (driver class 2, protocol type VIDEO (2)) and the reply
 * record of KVIDEO_OP_DIRECT_INFO (the op number lives in
 * kos/protocol.h).  While a program holds the object exclusively
 * (KOS_BIND_EXCLUSIVE), the driver answers KVIDEO_OP_DIRECT_INFO with a
 * struct kvideo_direct_info: the backend kind, what the exclusive
 * holder may do with the hardware directly, and the ports it writes.
 * Any other caller gets -KOS_EBUSY.
 *
 * The ports are listed one by one: the ABI does not assume they are
 * contiguous on another board.  Applications consume the record through
 * libgfx (kos/gfx.h); these constants are the backend contract.
 */

#ifndef KOS_KVIDEO_H
#define KOS_KVIDEO_H

#include "kos/types.h"

/* KVIDEO_OP_DIRECT_INFO's reply payload.  The record fits one IPC
 * reply; the frame's remaining payload bytes are zero. */
struct kvideo_direct_info {
	uint8_t version;              /* KVIDEO_DIRECT_INFO_VERSION    */
	uint8_t backend;              /* KVIDEO_BACKEND_*              */
	uint16_t flags;               /* KVIDEO_* capability bits      */
	uint16_t vram_kib;            /* video memory the backend sees */
	uint8_t data_port;            /* VRAM data                     */
	uint8_t command_port;         /* registers and status          */
	uint8_t palette_port;         /* palette entries               */
	uint8_t indirect_port;        /* command-engine registers      */
	uint8_t config_port;          /* the configuration latch       */
	uint8_t config_initial;       /* the latch value the console
	                               * uses (its interrupt route and
	                               * the porch bit)                */
	uint8_t reserved[7];          /* 0                             */
};

#define KVIDEO_DIRECT_INFO_VERSION 1u

/* struct kvideo_direct_info.backend values. */
#define KVIDEO_BACKEND_V9958 1u

/* struct kvideo_direct_info.flags bits: what the exclusive holder may
 * do with the reported ports. */
#define KVIDEO_DIRECT_IO 0x0001u /* the ports are the holder's to write */
#define KVIDEO_HW_WAIT   0x0002u /* R#25 WTE hardware WAIT is on        */
#define KVIDEO_PALETTE   0x0004u /* the VDP has a programmable palette  */
#define KVIDEO_YJK       0x0008u /* the VDP has the YJK colour mode     */

#if defined(__SDCC)
_Static_assert(sizeof(struct kvideo_direct_info) == 19,
	       "kvideo_direct_info is a 19-byte record");
_Static_assert(__builtin_offsetof(struct kvideo_direct_info, flags) == 2 &&
	       __builtin_offsetof(struct kvideo_direct_info, vram_kib) == 4 &&
	       __builtin_offsetof(struct kvideo_direct_info, data_port) == 6 &&
	       __builtin_offsetof(struct kvideo_direct_info,
				  config_initial) == 11,
	       "kvideo_direct_info layout");
#endif /* __SDCC */

#endif /* KOS_KVIDEO_H */
