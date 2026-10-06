/* ZephyrOS M10 extended-memory syscall services (kernel-internal, 33c).
 *
 * Private contract between the dispatcher (kernel/syscall.c) and the
 * service bodies (kernel/syscall/xmem.c).  The numbers and argument
 * blocks are the native ABI (abi/zos/syscall.h, abi/zos/xmem.h).
 */

#ifndef ZEPHYROS_KERNEL_SYSCALL_XMEM_H
#define ZEPHYROS_KERNEL_SYSCALL_XMEM_H

#include "zos/types.h"

/* Dispatch one number in 20h-26h with the caller's execution bank and the
 * raw argument-block pointer. */
void zos_xmem_dispatch(uint8_t number, uint8_t bank, uint16_t block);

/* Release every runtime object owned by pid (process exit/rollback). */
void zos_xmem_release_pid(uint8_t pid);

/* SYS_FAR_CALL service body (kernel/syscall/far.c, package 33d). */
void zos_far_call(uint8_t bank, uint16_t block);

/* 1 while a cross-bank far call is active for the running process; used
 * to reject recursive SYS_FAR_CALL and SYS_EXEC from far code. */
uint8_t zos_far_is_active(void);

#endif /* ZEPHYROS_KERNEL_SYSCALL_XMEM_H */
