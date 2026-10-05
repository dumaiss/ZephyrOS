; -----------------------------------------------------------------------------
; fschild entry stub (M9 exec acceptance child, test artifact).
;
; First _CODE object of the fschild link, so the C entry lands at 0100h and
; the packed header carries entry_offset 0.  The launch gate enters here in
; mode 10 with HL = 0000h (ZEX v1 has no startup block).
; -----------------------------------------------------------------------------

	.module fschild_entry

	.globl ch_entry
	.globl _ch_main
	.globl _zex_exit

	.area _CODE

ch_entry::
	ld	a,h
	or	l
	jr	nz,ch_entry_bad
	call	_ch_main
	; _ch_main must exit via zex_exit; reaching here is a test bug.
	ld	hl,#0x01FE
	call	_zex_exit
	di
001$:
	halt
	jr	001$

ch_entry_bad:
	ld	hl,#0x01FF
	call	_zex_exit
	di
002$:
	halt
	jr	002$
