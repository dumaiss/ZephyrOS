/* ZephyrOS IPC syscall dispatcher interface (kernel-private).
 *
 * The stable numbers and packed argument blocks are the public native ABI
 * (abi/zos/ipc.h); this header is the private seam between the syscall
 * dispatcher (kernel/syscall.c) and the IPC bodies (kernel/syscall/ipc.c).
 */

#ifndef ZEPHYROS_KERNEL_SYSCALL_IPC_H
#define ZEPHYROS_KERNEL_SYSCALL_IPC_H

#include "zos/types.h"

/* SYS_IPC_LOOKUP / SYS_IPC_CALL bodies.  `block` is the raw argument-block
 * pointer from the register ABI; `bank` is the caller's execution bank. */
void zos_ipc_dispatch(uint8_t number, uint8_t bank, uint16_t block);

#endif /* ZEPHYROS_KERNEL_SYSCALL_IPC_H */
