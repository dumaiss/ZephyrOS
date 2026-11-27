/* KonaOS nucleus: the real-time interrupt (architecture 4.12, abi/kos/rt.h).
 * Cold syscall bodies, the boot-time vectors and the release hooks. */

#ifndef KOS_KERNEL_SYSCALL_RT_H
#define KOS_KERNEL_SYSCALL_RT_H

#include "kos/types.h"
#include "memory.h"

struct kos_bank_copy_req;

/* Boot: the idle vector, and JP KOS_RT_VECTOR at 0066h in banks 0-6
 * (Mk2: the page-0 images carry the entry, the bootstrap's and exec's). */
void kos_rt_boot(void);

#if !defined(KOS_BOARD_MK2)
/* Write the NMI entry into one bank's page zero (exec builds each child's
 * page zero with it).  0, or the copy gate's error. */
uint8_t kos_rt_page_zero(uint8_t bank);

/* An auxiliary-bank copy (xmem objects, EXE segments): the mode-11
 * window from 6000h up, the mode-10 gate below it - which maps the bank at
 * 0000h, so the object's bytes at 0066h-0068h are kept aside and the NMI
 * entry stays in place.  The request is kos_bank_copy's.  0, or the
 * gate's error. */
uint8_t kos_aux_copy(const struct kos_bank_copy_req *req);
#endif

/* Non-zero while a program holds the interrupt: the ROM mode (the
 * recovery store, reboot) is refused then.  It reads the vector itself,
 * in common memory, so fixed-core code may use it with any window. */
#define KOS_RT_ROUTED() \
	(*(volatile const uint16_t *)(KOS_RT_VECTOR + 1u) != KOS_RT_IDLE)

/* Release whatever `pid` holds (process exit and failed-load rollback). */
void kos_rt_release_process(uint8_t pid);

#if defined(KOS_BOARD_MK2)
/* SYS_PERSONALITY's teardown: whoever holds the interrupt loses it, the
 * vector goes back to the idle RETN. */
void kos_rt_quiesce(void);
#endif

/* SYS_RT_REGISTER / _RELEASE / _INFO bodies. */
void kos_rt_register(uint16_t block);
void kos_rt_release(void);
void kos_rt_info(uint16_t block);

#endif /* KOS_KERNEL_SYSCALL_RT_H */
