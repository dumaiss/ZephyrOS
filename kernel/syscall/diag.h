/* KonaOS diagnostic syscalls (kernel-internal interface).
 *
 * The stable syscall numbers are declared in abi/kos/syscall.h; this
 * header is the private contract between
 * the dispatcher (kernel/syscall.c), the diagnostic bodies
 * (kernel/syscall/diag.c), and the bank-7 port helpers
 * (kernel/syscall/diag_io.asm).  It is not part of the native ABI.
 *
 * Every driver validates its complete argument block before touching
 * caller memory or device state, publishes its result through the common
 * dispatcher result state, and leaves failures side-effect free.  Memory
 * transfers reuse the checked copy gate (kernel/memory/user_copy.asm),
 * which restores the exact active latch, SP, and IFF state; the port
 * helpers do not modify the latch at all.
 */

#ifndef KONAOS_KERNEL_SYSCALL_DIAG_H
#define KONAOS_KERNEL_SYSCALL_DIAG_H

#include "kos/diag.h"

/* Dispatcher helpers shared with kernel/syscall.c: the argument-window
 * rule and the result publication state (written by the asm gate). */
uint8_t kos_syscall_block_ok(uint16_t addr, uint16_t len);
void kos_syscall_success(uint16_t result);
void kos_syscall_error(uint8_t err);

/* Copy-gate helpers shared with every syscall body that moves caller
 * bytes.  They move one bounded chunk between the caller buffer and the
 * common-resident staging buffer; the driver owns the chunking.  The
 * buffer's bank follows the buffer-identity rule (kernel/syscall.c
 * `kos_syscall_buffer_bank`), so an address behind the mode-10 aperture
 * reads or writes the far bank.  `kos_syscall_buffer_ok` checks the
 * 0100h-DFFFh user-buffer window and refuses a buffer crossing the
 * aperture boundary.  KOS_SYSCALL_STAGE_SIZE mirrors
 * kernel/memory/user_copy.asm. */
#define KOS_SYSCALL_STAGE_SIZE 64u
uint8_t kos_syscall_bank_read(uint16_t user_src, uint16_t length);
uint8_t kos_syscall_bank_write(uint16_t user_dst, uint16_t length);
uint8_t kos_syscall_buffer_ok(uint16_t addr, uint16_t len);
uint8_t kos_syscall_buffer_bank(uint16_t addr);

/* Driver bodies; each takes the raw argument-block pointer from the
 * register ABI (already range-checked only by the window rule inside). */
void kos_diag_mem_read(uint16_t block);
void kos_diag_mem_write(uint16_t block);
void kos_diag_io_in(uint16_t block);
void kos_diag_io_out(uint16_t block);
void kos_diag_bank_info(void);
void kos_diag_system_info(uint16_t block);

/* Bank-7 port helpers (kernel/syscall/diag_io.asm).  The caller has
 * already rejected the ladder-aliased ports 00h-0Fh. */
struct kos_diag_hw_port {
	uint8_t port;
	uint8_t value;
};

uint8_t kos_diag_hw_in(const struct kos_diag_hw_port *port);
void kos_diag_hw_out(const struct kos_diag_hw_port *port);

#endif /* KONAOS_KERNEL_SYSCALL_DIAG_H */
