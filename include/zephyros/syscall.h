/* ZephyrOS native syscall interface.
 *
 * The only stable interface is the documented register ABI in docs/abi.md:
 *
 *     RST 30h
 *     entry:  A  = syscall number
 *             HL = pointer to syscall-specific argument block
 *     return: carry clear = success, carry set = error
 *             HL = primary result
 *             A  = error code when carry is set
 *     preserved: BC, DE, IX, IY; SP and the selected bank are restored
 *     unspecified: other flags; alternate registers are not part of M1
 *
 * M1 returns ZOS_ENOSYS for every syscall number.  C names, argument
 * layout, and SDCC calling conventions are private and must not be
 * exposed to syscall callers.
 */

#ifndef ZEPHYROS_SYSCALL_H
#define ZEPHYROS_SYSCALL_H

#include "zephyros/types.h"
#include "zephyros/errno.h"

#if ZOS_SYSCALL_SELFTEST
/* Internal target-side self-test driver (build option only, not ABI). */
void zos_run_syscall_selftest(void);
#endif

#endif /* ZEPHYROS_SYSCALL_H */
