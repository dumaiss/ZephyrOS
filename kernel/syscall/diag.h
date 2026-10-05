/* ZephyrOS M7 diagnostic syscall services (kernel-internal interface).
 *
 * The stable numbers and packed argument blocks are declared in
 * include/zephyros/syscall.h; this header is the private contract between
 * the M7 dispatcher (kernel/syscall.c), the diagnostic service bodies
 * (kernel/syscall/diag.c), and the bank-7 port helpers
 * (kernel/syscall/diag_io.asm).  It is not part of the native ABI.
 *
 * Every service validates its complete argument block before touching
 * caller memory or device state, publishes its result through the common
 * dispatcher result state, and leaves failures side-effect free.  Memory
 * transfers reuse the M6 checked copy gate (kernel/memory/user_copy.asm),
 * which restores the exact active latch, SP, and IFF state; the port
 * helpers do not modify the latch at all.
 */

#ifndef ZEPHYROS_KERNEL_SYSCALL_DIAG_H
#define ZEPHYROS_KERNEL_SYSCALL_DIAG_H

#include "zephyros/syscall.h"

/* Dispatcher helpers shared with kernel/syscall.c: the argument-window
 * rule and the result publication state (written by the asm gate). */
uint8_t zos_syscall_block_ok(uint16_t addr, uint16_t len);
void zos_syscall_success(uint16_t result);
void zos_syscall_error(uint8_t err);

/* Copy-gate helpers shared with the M9 filesystem services
 * (kernel/syscall/fs.c).  They move one bounded chunk between the caller
 * bank and the common-resident staging buffer; the fs service owns the
 * chunking.  `zos_syscall_buffer_ok` checks the 0100h-DFFFh user-buffer
 * window.  ZOS_SYSCALL_STAGE_SIZE mirrors kernel/memory/user_copy.asm. */
#define ZOS_SYSCALL_STAGE_SIZE 64u
uint8_t zos_syscall_bank_read(uint8_t bank, uint16_t user_src,
			      uint16_t length);
uint8_t zos_syscall_bank_write(uint8_t bank, uint16_t user_dst,
			       uint16_t length);
uint8_t zos_syscall_buffer_ok(uint16_t addr, uint16_t len);

/* Service bodies; each takes the raw argument-block pointer from the
 * register ABI (already range-checked only by the window rule inside). */
void zos_diag_mem_read(uint16_t block);
void zos_diag_mem_write(uint16_t block);
void zos_diag_io_in(uint16_t block);
void zos_diag_io_out(uint16_t block);
void zos_diag_bank_info(void);
void zos_diag_system_info(uint16_t block);

/* Bank-7 port helpers (kernel/syscall/diag_io.asm).  The caller has
 * already rejected the ladder-aliased ports 00h-0Fh. */
struct zos_diag_hw_port {
	uint8_t port;
	uint8_t value;
};

uint8_t zos_diag_hw_in(const struct zos_diag_hw_port *port);
void zos_diag_hw_out(const struct zos_diag_hw_port *port);

#endif /* ZEPHYROS_KERNEL_SYSCALL_DIAG_H */
