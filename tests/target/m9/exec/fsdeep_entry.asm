; -----------------------------------------------------------------------------
; fsdeep entry stub (M9 exec depth-probe program, test artifact).
; -----------------------------------------------------------------------------

	.module fsdeep_entry

	.globl dp_entry
	.globl _dp_main
	.globl _zex_exit

	.area _CODE

dp_entry::
	ld	a,h
	or	l
	jr	nz,dp_entry_bad
	call	_dp_main
	ld	hl,#0x02FE
	call	_zex_exit
	di
001$:
	halt
	jr	001$

dp_entry_bad:
	ld	hl,#0x02FF
	call	_zex_exit
	di
002$:
	halt
	jr	002$
