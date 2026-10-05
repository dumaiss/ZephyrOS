; -----------------------------------------------------------------------------
; fstest entry stub (M9 filesystem syscall acceptance, test artifact).
;
; First _CODE object of the fstest link, so the C entry lands at 0100h and
; the packed header carries entry_offset 0.  The process launch gate enters
; here in mode 10 with interrupts enabled and HL = 0000h (ZEX v1 has no
; startup block); a nonzero HL is a launch contract violation.
; -----------------------------------------------------------------------------

	.module fstest_entry

	.globl ft_entry
	.globl _ft_main
	.globl _zex_exit

	.area _CODE

ft_entry::
	ld	a,h
	or	l
	jr	nz,ft_entry_bad
	call	_ft_main
	; _ft_main exits via zex_exit; reaching here is a test bug.
	ld	hl,#0x00FE
	call	_zex_exit
	di
001$:
	halt
	jr	001$

ft_entry_bad:
	ld	hl,#0x00FF
	call	_zex_exit
	di
002$:
	halt
	jr	002$
