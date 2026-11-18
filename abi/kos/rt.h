/* KonaOS native real-time interrupt ABI (public native ABI).
 *
 * Architecture 4.12: one program at a time may own the real-time
 * interrupt - a handler the IO controller's RT clock runs at a fixed rate
 * through the Z80's NMI, so no masked section of the system can hold it
 * back.  The nucleus lends its owner the RT region of common memory,
 * which every mapping shows: the handler and everything it touches live
 * there.
 *
 *   SYS_RT_REGISTER  take the interrupt: the handler's address
 *   SYS_RT_RELEASE   give it back
 *   SYS_RT_INFO      the region and whether the interrupt is held
 *
 * Registering only routes the NMI vector; the rate is the IO controller's
 * (`/dev/rtclock`, KIOC_OP_RT_CLOCK).  The SDK's rt_open does both.  A
 * terminating program's registration is released with its handles, so
 * its clock stops too.
 *
 * The handler contract: entered through the NMI with the interrupted
 * code's registers, stack, mapping and interrupt state; it preserves
 * every register it uses, pushes no more than KOS_RT_STACK_MAX bytes,
 * makes no syscall, changes no mapping and returns with RETN.  It must
 * finish well inside one period.
 *
 * While the interrupt is held the recovery store (`/system`) and
 * SYS_REBOOT answer KOS_EBUSY: both run the ROM mode, in which an NMI
 * cannot be served.
 */

#ifndef KOS_RT_H
#define KOS_RT_H

#include "kos/types.h"

#define KOS_SYSCALL_RT_REGISTER 0x10u
#define KOS_SYSCALL_RT_RELEASE  0x11u
#define KOS_SYSCALL_RT_INFO     0x12u

/* The RT region: common memory lent to the owner. */
#define KOS_RT_REGION_BASE 0xF000u
#define KOS_RT_REGION_SIZE 0x0D00u      /* F000h-FCFFh */

/* The rates the RT clock accepts, Hz. */
#define KOS_RT_RATE_MIN 1000u
#define KOS_RT_RATE_MAX 32000u

/* The handler's stack budget on the interrupted stack, bytes: one push
 * (the NMI's own return address comes on top).  The system's smallest
 * stacks are sized for a maskable interrupt plus this tick. */
#define KOS_RT_STACK_MAX 2u

/* The last maskable interrupt's source: a common byte the nucleus writes
 * at every maskable interrupt's entry and an NMI never touches.  No source
 * is KOS_IRQ_NO_SOURCE, so a wait that must not end on a tick (idle) can
 * store that value and halt until the byte changes. */
#define KOS_IRQ_LAST_SOURCE 0xFE06u
#define KOS_IRQ_NO_SOURCE   0xFFu

/* SYS_RT_REGISTER 10h {handler:u16} -> 0.  The handler must lie in the RT
 * region (KOS_EINVAL otherwise); KOS_EBUSY while another program holds the
 * interrupt.  Registering again replaces the caller's own handler. */
struct kos_rt_register_args {
	uint16_t handler;
};

/* SYS_RT_RELEASE 11h {} -> 0.  A caller that does not hold the interrupt
 * changes nothing. */

/* SYS_RT_INFO 12h {buffer:u16} -> 0; the record into `buffer`. */
struct kos_rt_info_args {
	uint16_t buffer;
};

#define KOS_RT_FREE   0u   /* nobody holds the interrupt */
#define KOS_RT_MINE   1u   /* the caller holds it        */
#define KOS_RT_OTHER  2u   /* another program holds it   */

struct kos_rt_info {
	uint16_t region_base;
	uint16_t region_size;
	uint8_t  held;        /* KOS_RT_FREE / _MINE / _OTHER */
	uint8_t  reserved[3];
};

#if defined(__SDCC)
_Static_assert(sizeof(struct kos_rt_register_args) == 2,
	       "rt register block must be 2 packed bytes");
_Static_assert(sizeof(struct kos_rt_info_args) == 2,
	       "rt info block must be 2 packed bytes");
_Static_assert(sizeof(struct kos_rt_info) == 8,
	       "rt info record must be 8 packed bytes");
#endif

#endif /* KOS_RT_H */
