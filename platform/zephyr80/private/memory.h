/* Zephyr-80 memory-map constants (C visible).
 *
 * Region geometry of the mode 10/11 machine: the page-zero/reset budget,
 * the bank-7 loadable/runtime areas, permanent common
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
 * must stay below KOS_RESET_TOP so that M1 can install RST 30h there. */
#define KOS_RESET_TOP               0x0030u
#define KOS_PAGE_ZERO_END           0x0100u

/* Bank-7 OS body, visible in mode 11 under decoder revision 13.  Mode 11
 * splits bank 7 into a 16 KiB fixed core and a 32 KiB cold half behind the
 * 6000h window; see memory.inc for the region diagram.  The fixed core's
 * loadable part ends at the linked end of the fixed code areas; its runtime
 * data begins at the derived data base, below the reserved stacks, and is
 * never loaded from ROM. */
#define KOS_BANK7_BASE              0x2000u
#define KOS_REV13_WINDOW_BASE       0x6000u
#define KOS_BANK7_RUNTIME_END       0xE000u
#define KOS_IRQ_ISR_STACK_BASE      0x5F00u
#define KOS_IRQ_ISR_STACK_TOP       0x6000u
#define KOS_IRQ_ISR_STACK_SIZE      0x0100u

/* Driver stack: 512 bytes in the fixed core, reserved now and used by the
 * IPC substrate from stage 3 (architecture 36.9, 36.20).  It grows down
 * from KOS_DRIVER_STACK_TOP, below the relocated ISR stack. */
#define KOS_DRIVER_STACK_BASE      0x5D00u
#define KOS_DRIVER_STACK_TOP       0x5F00u
#define KOS_DRIVER_STACK_SIZE      0x0200u

/* Bank-7 data alignment.  The two derived runtime-data bases (the fixed
 * core's, below the driver stack, and the cold half's, straight after
 * the linked window code) are aligned to this granule so the linker's
 * segment bases are reproducible. */
#define KOS_BANK7_DATA_ALIGN        0x0200u

/* Permanent common code/data in physical SRAM bank 0.  KOS_COMMON_CODE_END
 * is the exclusive end of ordinary common content; the IM2 vector page and
 * IRQ transition state reservations (FD00h-FEFFh) are declared in irq.h.
 *
 * KOS_COMMON_CEILING is the architectural ceiling on the packed
 * `_COMMON` + `_COMMON_DATA` run (work package 30c, from the 30b audit):
 * common memory is the scarcest region.  The M9 reservation held back for
 * M10's far-call state was released to M10's transitions and IPC by work
 * package 43 (architecture 36.20), so the whole ceiling is the limit.
 * It may not be raised to make a package fit. */
#define KOS_COMMON_BASE             0xE000u
#define KOS_COMMON_CODE_END         0xFD00u
#define KOS_COMMON_STACK_BASE       0xFF00u
#define KOS_COMMON_CEILING          0x0C00u

/* Common boot/runtime stack: FF00h-FFFFh, grown down from FFFFh.
 * Initial SP = 0000h (top-exclusive at 10000h). */
#define KOS_STACK_INIT_SP           0x0000u

/* Common gate stack used only while crossing between mode 10 and
 * mode 11.  Lives in the common data area, below the boot stack.  It
 * carries the whole syscall handler chain, including a module class op
 * that runs a full device transaction (M8 console read: HID -> IOC -> SIO
 * needs about 180 bytes) and the nested exec (SYS_EXEC -> ROMFS read ->
 * provider and driver transaction, measured at 287 bytes with the M10
 * filesystem driver), so it is sized well above the M1 dispatch set.
 * The transition state cells sit directly below the stack base: a chain
 * that overruns the reservation overwrites them, so the size must stay
 * above the deepest measured chain. */
#define KOS_GATE_STACK_SIZE         384u

/* ROM geometry of the programmed 512 KiB image: eight 64-KiB pages.
 * Pages 0 and 1 are the boot pages copied to SRAM by the reset copier;
 * pages 2-7 carry the read-only recovery store (ROMFS). */
#define KOS_ROM_PAGE_SIZE           0x10000u
#define KOS_ROM_PAGE_COUNT          8u
#define KOS_BOOT_IMAGE_SIZE         0x20000u
#define KOS_ROM_IMAGE_SIZE          0x80000u

/* ROMFS recovery-store geometry (platform assignment).  Only the
 * 0000h-BFFFh window of each recovery page is part of the logical
 * stream; C000h-FFFFh is never addressed as file data.  While a page is
 * visible the machine is in mode 00, so C000h-FFFFh reads come from the
 * ROM page as well; the fixed ROM-access stub mirror lives there.
 * Capacity = KOS_ROMFS_PAGE_COUNT x window = 288 KiB. */
#define KOS_ROMFS_FIRST_PAGE        2u
#define KOS_ROMFS_PAGE_COUNT        6u
#define KOS_ROMFS_WINDOW_SIZE       0xC000u
#define KOS_ROMFS_CAPACITY          0x48000u

/* ROM-access transition block in permanent common memory
 * (kernel/romfs/rom_access.asm).  A two-byte trampoline switches to
 * mode 00 at KOS_ROM_ACCESS_BASE-2 and falls through into the mirrored
 * stub at KOS_ROM_ACCESS_BASE: LDIR plus the latch-restoring OUT, then
 * a JP back to ordinary common code.  The stub is byte-identical in
 * common SRAM and in every recovery ROM page, so instruction fetch
 * survives the mode change. */
#define KOS_ROMFS_STAGING_SIZE      128u
#define KOS_ROM_ACCESS_BASE         0xEF00u
#define KOS_ROM_ACCESS_SIZE         8u

#endif /* ZEPHYR80_MEMORY_H */
