/* ZephyrOS kernel-private memory-control primitives (kernel/memory).
 *
 * The declarative board constants (latch fields, mode values, memory
 * geometry) stay in platform/zephyr80/.  These are the internal,
 * assembly-backed latch/bank primitives of the M1 memory subsystem: a
 * kernel mechanism, not a stable ABI and not part of the module-facing
 * surface.  Kernel subsystems and acceptance code include it from this
 * explicit owning path.
 *
 * Latch layout: D2..D0 bank, D4:D3 mode, D7:D5 page.
 * platform_bank_set() preserves ROM-page and mode bits and rejects bank
 * 7 (application banks are 0-6; bank 7 is reserved to ZephyrOS).  A
 * rejected value leaves the latch untouched.  All of these are
 * mode-preserving and safe to call from bank-7 code while mode 11 is
 * active.
 *
 * The mode 10/11 transitions are assembly gates with explicit
 * common-memory continuations (zos_enter_os_mode / zos_leave_os_mode in
 * kernel/memory/banking.asm), not C functions: a C return address in
 * 2000h-DFFFh would disappear when the mode changes.
 */

#ifndef ZEPHYROS_KERNEL_MEMORY_BANKING_H
#define ZEPHYROS_KERNEL_MEMORY_BANKING_H

#include "zos/types.h"

/* Raw latch value (all fields). */
uint8_t platform_memctl_read(void);
void platform_memctl_write(uint8_t value);

/* Selected SRAM bank field (D2..D0). */
uint8_t platform_bank_get(void);
void platform_bank_set(uint8_t bank);

#endif /* ZEPHYROS_KERNEL_MEMORY_BANKING_H */
