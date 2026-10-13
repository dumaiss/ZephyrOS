; -----------------------------------------------------------------------------
; Zephyr-80 reset / ROM-to-SRAM boot copier.
;
; Z80 reset enters here at 0000h with the banking latch at 00h (ROM page 0,
; mode 00, bank 0).  In mode 00, instruction fetch and data reads come from
; the selected ROM page while writes target the selected SRAM bank, so two
; full 64 KiB LDIR copies move ROM page 0 -> SRAM bank 0 and ROM page 1 ->
; SRAM bank 7 without any stack.
;
; Instruction fetch continues across the latch change to page 1, so the
; page-1 image carries a byte-identical copy of this copier at the same
; addresses (enforced by tools/pack_image.py).  The final latch change to
; 10h continues execution from the identical copy in SRAM bank 0.
;
; The whole copier must remain below 0030h so that the kernel can install RST 30h
; in page zero.  Interrupts stay disabled; EI is never executed.
;
; Output state: mode 10 over bank 0, interrupts disabled, no stack, jumping
; to _boot_common_entry in the permanent common region (bank 0 E000h+).
; -----------------------------------------------------------------------------

	.module reset
	.include "hardware.inc"
	.include "memory.inc"

	.globl _boot_common_entry

	.area _RESET (ABS)
	.org 0x0000

_reset::
	di

	; Canonicalize: page 0, mode 00, bank 0.
	ld	a,#KOS_MODE_BOOT
	out	(KOS_MEMCTL_PORT),a

	; Copy ROM page 0 -> SRAM bank 0 (65536 transfers).
	ld	hl,#0x0000
	ld	de,#0x0000
	ld	bc,#0x0000
	ldir

	; Select ROM page 1, mode 00, SRAM bank 7: 20h | 00h | 07h = 27h.
	; The next instruction fetch comes from ROM page 1 at the same
	; address; its bytes here are identical (mirrored copier).
	ld	a,#(KOS_BOOT_OS_ROM_PAGE << KOS_ROM_PAGE_SHIFT) | KOS_MODE_BOOT | KOS_BOOT_OS_BANK
	out	(KOS_MEMCTL_PORT),a

	; Copy ROM page 1 -> SRAM bank 7 (65536 transfers).
	ld	hl,#0x0000
	ld	de,#0x0000
	ld	bc,#0x0000
	ldir

	; Application mode over bank 0: 10h | 00h = 10h.  Execution continues
	; from the identical copier bytes in SRAM bank 0.
	ld	a,#KOS_MODE_APPLICATION
	out	(KOS_MEMCTL_PORT),a

	jp	_boot_common_entry

_reset_end::

	.ifgt (_reset_end - _reset) - KOS_RESET_TOP
	.error 1	; reset copier overruns the KOS_RESET_TOP page-zero budget
	.endif
