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
 * ends at the linked __kernel_code_end; the runtime area begins at the
 * derived data base and is never loaded from ROM.  ZOS_BANK7_LOAD_END is
 * the architectural ceiling on the loadable end, not a fixed boundary. */
#define ZOS_BANK7_BASE              0x2000u
#define ZOS_BANK7_LOAD_END          0xC000u
#define ZOS_BANK7_RUNTIME_END       0xDF00u
#define ZOS_IRQ_ISR_STACK_BASE      0xDF00u
#define ZOS_IRQ_ISR_STACK_TOP       0xE000u
#define ZOS_IRQ_ISR_STACK_SIZE      0x0100u

/* Driver arena.  The single mechanism derives the start from the aligned
 * end of the linked kernel and the end from the runtime-data size, exporting
 * __kernel_code_end / __driver_arena_start / __driver_arena_end through
 * kernel/module/arena.asm and the generated build/geometry/arena_geometry.inc;
 * the probe link places no arena.  Only the build-time contract lives here:
 * the granule size, and the production minimum that mapcheck enforces.  The
 * runtime data base is never part of the allocator. */
#define ZOS_DRIVER_GRANULE_SIZE     0x0200u
#define ZOS_DRIVER_ARENA_MIN_SIZE   0x4000u
#define ZOS_DRIVER_ARENA_MIN_GRANULES 32u

/* Permanent common code/data in physical SRAM bank 0.  ZOS_COMMON_CODE_END
 * is the exclusive end of ordinary common content; the IM2 vector page and
 * IRQ transition state reservations (FD00h-FEFFh) are declared in irq.h.
 *
 * ZOS_COMMON_CEILING is the architectural ceiling on the packed
 * `_COMMON` + `_COMMON_DATA` run (work package 30c, from the 30b audit):
 * common memory is the scarcest region.  ZOS_COMMON_M10_RESERVATION is held
 * back for M10's far-call gate/state (target architecture 2.13-2.15), so
 * M9 enforces `used <= ceiling - reservation`.  Neither may be raised to
 * make a package fit. */
#define ZOS_COMMON_BASE             0xE000u
#define ZOS_COMMON_CODE_END         0xFD00u
#define ZOS_COMMON_STACK_BASE       0xFF00u
#define ZOS_COMMON_CEILING          0x0C00u
#define ZOS_COMMON_M10_RESERVATION  0x0200u

/* Common boot/runtime stack: FF00h-FFFFh, grown down from FFFFh.
 * Initial SP = 0000h (top-exclusive at 10000h). */
#define ZOS_STACK_INIT_SP           0x0000u

/* Common gate stack used only while crossing between mode 10 and
 * mode 11.  Lives in the common data area, below the boot stack.  It
 * carries the whole syscall handler chain, including a module class op
 * that runs a full device transaction (M8 console read: HID -> IOC -> SIO
 * needs about 180 bytes), so it is sized well above the M1 dispatch set. */
#define ZOS_GATE_STACK_SIZE         256u

/* ROM geometry of the programmed 512 KiB image: eight 64-KiB pages.
 * Pages 0 and 1 are the boot pages copied to SRAM by the reset copier;
 * pages 2-7 carry the read-only recovery store (ROMFS). */
#define ZOS_ROM_PAGE_SIZE           0x10000u
#define ZOS_ROM_PAGE_COUNT          8u
#define ZOS_BOOT_IMAGE_SIZE         0x20000u
#define ZOS_ROM_IMAGE_SIZE          0x80000u

/* ROMFS recovery-store geometry (platform assignment).  Only the
 * 0000h-BFFFh window of each recovery page is part of the logical
 * stream; C000h-FFFFh is never addressed as file data.  While a page is
 * visible the machine is in mode 00, so C000h-FFFFh reads come from the
 * ROM page as well; the fixed ROM-access stub mirror lives there.
 * Capacity = ZOS_ROMFS_PAGE_COUNT x window = 288 KiB. */
#define ZOS_ROMFS_FIRST_PAGE        2u
#define ZOS_ROMFS_PAGE_COUNT        6u
#define ZOS_ROMFS_WINDOW_SIZE       0xC000u
#define ZOS_ROMFS_CAPACITY          0x48000u

/* ROM-access transition block in permanent common memory
 * (kernel/romfs/rom_access.asm).  A two-byte trampoline switches to
 * mode 00 at ZOS_ROM_ACCESS_BASE-2 and falls through into the mirrored
 * stub at ZOS_ROM_ACCESS_BASE: LDIR plus the latch-restoring OUT, then
 * a JP back to ordinary common code.  The stub is byte-identical in
 * common SRAM and in every recovery ROM page, so instruction fetch
 * survives the mode change. */
#define ZOS_ROMFS_STAGING_SIZE      128u
#define ZOS_ROM_ACCESS_BASE         0xEF00u
#define ZOS_ROM_ACCESS_SIZE         8u

#endif /* ZEPHYR80_MEMORY_H */
