/* Zephyr-80 memory-map constants (C visible).
 *
 * Region geometry of the mode 10/11 machine: the page-zero/reset budget,
 * the bank-7 loadable/runtime areas, the M4 driver arena, permanent common
 * memory, stacks, and the ROM image geometry.  The bank-7 ISR stack is part
 * of the bank-7 region budget; IM2 vector-page and IRQ transition wiring
 * constants live in irq.h/.inc.
 *
 * Keep this file value-identical with memory.inc; the host parity test
 * (tests/test_m1.py) enforces it.  Hardware ports and register constants
 * stay in hardware.h/.inc.
 */

#ifndef ZEPHYR80_MEMORY_H
#define ZEPHYR80_MEMORY_H

/* Page-zero/reset reservation in ROM page 0.  The stackless reset copier
 * must stay below ZOS_RESET_TOP so that M1 can install RST 30h there. */
#define ZOS_RESET_TOP               0x0030u
#define ZOS_PAGE_ZERO_END           0x0100u

/* Bank-7 OS body, visible in mode 11.  The loadable (ROM-initialized) part
 * must end below ZOS_DRIVER_ARENA_BASE; C000h-DFFFh is zero-initialized
 * runtime area that is never loaded from ROM. */
#define ZOS_BANK7_BASE              0x2000u
#define ZOS_BANK7_LOAD_END          0xC000u
#define ZOS_BANK7_RUNTIME_END       0xDF00u
#define ZOS_IRQ_ISR_STACK_BASE      0xDF00u
#define ZOS_IRQ_ISR_STACK_TOP       0xE000u
#define ZOS_IRQ_ISR_STACK_SIZE      0x0100u

/* M4 bank-7 driver arena: 16 contiguous 512-byte granules reserved between
 * the loadable kernel image and the zero-initialized runtime area.  Loader
 * code must use the linked __driver_arena_start/__driver_arena_end symbols,
 * not these values. */
#define ZOS_DRIVER_ARENA_BASE       0xA000u
#define ZOS_DRIVER_ARENA_END        0xC000u
#define ZOS_DRIVER_GRANULE_SIZE     0x0200u
#define ZOS_DRIVER_GRANULES         16u

/* Permanent common code/data in physical SRAM bank 0.  ZOS_COMMON_CODE_END
 * is the exclusive end of ordinary common content; the IM2 vector page and
 * IRQ transition state reservations (FD00h-FEFFh) are declared in irq.h. */
#define ZOS_COMMON_BASE             0xE000u
#define ZOS_COMMON_CODE_END         0xFD00u
#define ZOS_COMMON_STACK_BASE       0xFF00u

/* Common boot/runtime stack: FF00h-FFFFh, grown down from FFFFh.
 * Initial SP = 0000h (top-exclusive at 10000h). */
#define ZOS_STACK_INIT_SP           0x0000u

/* Small common gate stack used only while crossing between mode 10 and
 * mode 11.  Lives in the common data area, below the boot stack. */
#define ZOS_GATE_STACK_SIZE         64u

/* ROM geometry of the 128 KiB boot image. */
#define ZOS_ROM_PAGE_SIZE           0x10000u
#define ZOS_BOOT_IMAGE_SIZE         0x20000u

#endif /* ZEPHYR80_MEMORY_H */
