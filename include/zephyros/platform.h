/* ZephyrOS platform interface.
 *
 * This header is the only platform entry point kernel code needs at M0:
 * public fixed-width types plus the Zephyr-80 hardware constants that belong
 * under platform/zephyr80/.  Kernel code must not hard-code port numbers or
 * board constants; it includes this header instead.
 */

#ifndef ZEPHYROS_PLATFORM_H
#define ZEPHYROS_PLATFORM_H

#include "zephyros/types.h"
#include "zephyr80/hardware.h"

/* --- M1 platform banking primitives ------------------------------------
 *
 * Internal ZephyrOS platform mechanisms (assembly-backed, common-resident),
 * not a stable ABI.  Latch layout: D2..D0 bank, D4:D3 mode, D7:D5 page.
 *
 * platform_bank_set() preserves ROM-page and mode bits and rejects bank 7
 * (application banks are 0-6; bank 7 is reserved to ZephyrOS).  A rejected
 * value leaves the latch untouched.  All of these are mode-preserving and
 * safe to call from bank-7 code while mode 11 is active.
 */

/* Raw latch value (all fields). */
uint8_t platform_memctl_read(void);
void platform_memctl_write(uint8_t value);

/* Selected SRAM bank field (D2..D0). */
uint8_t platform_bank_get(void);
void platform_bank_set(uint8_t bank);

/* Mode 10/11 transitions are assembly gates with explicit common-memory
 * continuations (zos_enter_os_mode / zos_leave_os_mode in
 * platform/zephyr80/banking.asm), not C functions: a C return address in
 * 2000h-DFFFh would disappear when the mode changes. */

#endif /* ZEPHYROS_PLATFORM_H */
