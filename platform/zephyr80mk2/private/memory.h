/* Zephyr-80 Mk2 memory-map constants (C visible).
 *
 * The common page (architecture 2.7.2): DAT entries 7 and 15 name the
 * same SRAM page, so logical E000h-FFFFh is the same memory in the system
 * and the working bank.  It keeps the Mk1 common layout (architecture
 * 2.4) and adds the DAT state (see memory.inc).  Keep this file
 * value-identical with memory.inc (parity-tested).
 */

#ifndef ZEPHYR80MK2_MEMORY_H
#define ZEPHYR80MK2_MEMORY_H

/* The low entry stubs every page-0 image carries (0000h-00FFh). */
#define KOS_PAGE_ZERO_END           0x0100u
#define KOS_NMI_ENTRY               0x0066u

/* The system DAT after the boot: entry 0 the system page-0 image (a
 * caller's page 0 for a syscall crossing), entries 1-4 the nucleus pages,
 * never remapped, entries 5-6 the copy windows, entry 7 the common page
 * (see memory.inc). */
#define KOS_NUCLEUS_BASE            0x2000u
#define KOS_NUCLEUS_END             0xA000u
#define KOS_NUCLEUS_FIRST_ENTRY     1u
#define KOS_NUCLEUS_ENTRIES         4u
#define KOS_SYSTEM_PAGE0            0x40u
#define KOS_NUCLEUS_FIRST_PAGE      0x41u
#define KOS_COMMON_PAGE             0x7Fu

/* The ROM image: 512 KiB, 64 pages; ROM pages 00h-07h hold the linked
 * image's logical pages, the recovery store follows (see memory.inc). */
#define KOS_ROM_IMAGE_SIZE          0x80000u
#define KOS_ROM_IMAGE_PAGES         0x08u
#define KOS_ROMFS_FIRST_PAGE        0x08u
#define KOS_ROMFS_PAGE_COUNT        36u
#define KOS_ROMFS_WINDOW_SIZE       0x2000u
#define KOS_ROMFS_CAPACITY          0x48000u
#define KOS_ROMFS_STAGING_SIZE      128u

/* The system bank's ISR stack: the top of logical page 2. */
#define KOS_IRQ_ISR_STACK_BASE      0x5F00u
#define KOS_IRQ_ISR_STACK_TOP       0x6000u
#define KOS_IRQ_ISR_STACK_SIZE      0x0100u

/* The common page and its packed-run ceiling. */
#define KOS_COMMON_BASE             0xE000u
#define KOS_COMMON_CEILING          0x0C00u
#define KOS_STACK_INIT_SP           0x0000u
#define KOS_GATE_STACK_SIZE         384u

/* The real-time interrupt's vector and its idle RETN. */
#define KOS_RT_VECTOR               0xEF08u
#define KOS_RT_IDLE                 0xEF0Bu

/* DAT state: the bank mirrors, the control mirror and the identity of
 * the image the working bank holds. */
#define KOS_DAT_STATE_BASE          0xEF10u
#define KOS_DAT_SYSTEM              0xEF10u
#define KOS_DAT_WORKING             0xEF18u
#define KOS_DAT_CONTROL             0xEF20u
#define KOS_DAT_CACHED              0xEF21u
#define KOS_DAT_STATE_SIZE          0x0013u

/* The nucleus's copy windows: two system entries it remaps to move bytes
 * between pages (an object, a caller's buffer, a program image) without
 * staging them.  No interrupt path uses them. */
#define KOS_DAT_COPY_SRC_ENTRY      5u
#define KOS_DAT_COPY_SRC_BASE       0xA000u
#define KOS_DAT_COPY_DST_ENTRY      6u
#define KOS_DAT_COPY_DST_BASE       0xC000u

/* The far window (logical 8000h-BFFFh): working entries 12-13. */
#define KOS_DAT_FAR_ENTRY           12u
#define KOS_DAT_FAR_BASE            0x8000u

#endif /* ZEPHYR80MK2_MEMORY_H */
