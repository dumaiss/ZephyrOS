; -----------------------------------------------------------------------------
; Zephyr-80 Mk2 hardware constants (assembly visible): the LS612 DAT mapper
; (architecture 2.7.2, 2.7.4).
;
; Sixteen 7-bit entries, the system bank (entries 0-7) and the working bank
; (entries 8-15), each translating one 8 KiB logical page; the control
; register selects the bank and enables mapping.  The peripheral cards are
; the Mk1 board's: their constants are platform/zephyr80/public/
; hardware.inc, included at the end, which leaves its CPU-board latch out
; on a board that defines KOS_DAT_CONTROL_PORT.
;
; Keep this file value-identical with hardware.h (parity-tested).
; -----------------------------------------------------------------------------

; --- Mapper ports ---

; Entry n is written at low I/O port n with I/O A15 = 0: OUT (n),A, or
; OUT (C),A with B = A, so the page is on both D0-D6 and A8-A14 (the
; entry inputs' wiring is open).
KOS_DAT_SYSTEM_PORT     = 0x00
KOS_DAT_WORKING_PORT    = 0x08
; The control register: BC = 800Fh, OUT (C),A (A15 = 1, low byte 0Fh).
KOS_DAT_CONTROL_PORT    = 0x800F

; --- Control register ---

KOS_DAT_BANK            = 0x01	; D0: 0 = system bank, 1 = working bank
KOS_DAT_MAP_ENABLE      = 0x02	; D1: 0 = pass (boot) view, 1 = mapped
KOS_DAT_CTL_SYSTEM      = 0x02	; mapped, system bank
KOS_DAT_CTL_WORKING     = 0x03	; mapped, working bank

; --- Pages ---

KOS_DAT_ENTRIES         = 8	; entries per bank, one per logical page
KOS_DAT_PAGE_SHIFT      = 13
KOS_DAT_PAGE_SIZE       = 0x2000
KOS_DAT_PAGE_MASK       = 0x7F
KOS_DAT_ROM_FIRST       = 0x00	; 00h-3Fh: the 512 KiB ROM
KOS_DAT_ROM_PAGES       = 0x40
KOS_DAT_SRAM_FIRST      = 0x40	; 40h-7Fh: the 512 KiB SRAM
KOS_DAT_SRAM_PAGES      = 0x40

; --- The peripheral cards (the Mk1 board's) ---

	.include "../../zephyr80/public/hardware.inc"
