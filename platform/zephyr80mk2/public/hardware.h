/* Zephyr-80 Mk2 hardware constants (C visible): the LS612 DAT mapper
 * (architecture 2.7.2, 2.7.4).
 *
 * Sixteen 7-bit entries, the system bank (entries 0-7) and the working
 * bank (entries 8-15), each translating one 8 KiB logical page; the
 * control register selects the bank and enables mapping.  The peripheral
 * cards are the Mk1 board's, with their constants in platform/zephyr80/
 * public.  Keep this file value-identical with hardware.inc
 * (parity-tested).
 */

#ifndef ZEPHYR80MK2_HARDWARE_H
#define ZEPHYR80MK2_HARDWARE_H

/* --- Mapper ports --- */

/* Entry n is written at low I/O port n with I/O A15 = 0, the page on both
 * D0-D6 and A8-A14 (the entry inputs' wiring is open). */
#define KOS_DAT_SYSTEM_PORT         0x00u
#define KOS_DAT_WORKING_PORT        0x08u
/* The control register: the 16-bit port 800Fh (A15 = 1, low byte 0Fh). */
#define KOS_DAT_CONTROL_PORT        0x800Fu

/* --- Control register --- */

#define KOS_DAT_BANK                0x01u /* D0: 0 system, 1 working */
#define KOS_DAT_MAP_ENABLE          0x02u /* D1: 0 pass, 1 mapped */
#define KOS_DAT_CTL_SYSTEM          0x02u
#define KOS_DAT_CTL_WORKING         0x03u

/* --- Pages --- */

#define KOS_DAT_ENTRIES             8u
#define KOS_DAT_PAGE_SHIFT          13u
#define KOS_DAT_PAGE_SIZE           0x2000u
#define KOS_DAT_PAGE_MASK           0x7Fu
#define KOS_DAT_ROM_FIRST           0x00u /* 00h-3Fh: the 512 KiB ROM */
#define KOS_DAT_ROM_PAGES           0x40u
#define KOS_DAT_SRAM_FIRST          0x40u /* 40h-7Fh: the 512 KiB SRAM */
#define KOS_DAT_SRAM_PAGES          0x40u

#endif /* ZEPHYR80MK2_HARDWARE_H */
