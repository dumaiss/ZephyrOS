/* ZephyrOS object/namespace syscall dispatcher interface (kernel-private).
 *
 * The stable numbers and packed argument blocks are the public native ABI
 * (abi/zos/object.h); this header is the private seam between the syscall
 * dispatcher (kernel/syscall.c) and the object and namespace bodies
 * (kernel/syscall/object.c).
 */

#ifndef ZEPHYROS_KERNEL_SYSCALL_OBJECT_H
#define ZEPHYROS_KERNEL_SYSCALL_OBJECT_H

#include "zos/types.h"

/* SYS_BIND / SYS_UNBIND / SYS_NS_ENUM bodies.  `block` is the raw
 * argument-block pointer from the register ABI; `bank` is the caller's
 * execution bank (the namespace enumeration writes there). */
void zos_object_dispatch(uint8_t number, uint8_t bank, uint16_t block);

#endif /* ZEPHYROS_KERNEL_SYSCALL_OBJECT_H */
