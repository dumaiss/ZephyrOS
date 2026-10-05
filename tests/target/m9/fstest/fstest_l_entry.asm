; -----------------------------------------------------------------------------
; fstest_l entry stub (M9 launcher-cleanup check, test artifact).
;
; A second native program exec'd after fstest.zex to prove the descriptor
; pool and CWD were released on exit.  Same contract as fstest_entry.asm.
; -----------------------------------------------------------------------------

	.module fstest_l_entry

	.globl ftl_entry
	.globl _ftl_main
	.globl _zex_exit

	.area _CODE

ftl_entry::
	ld	a,h
	or	l
	jr	nz,ftl_entry_bad
	call	_ftl_main
	ld	hl,#0x00FE
	call	_zex_exit
	di
001$:
	halt
	jr	001$

ftl_entry_bad:
	ld	hl,#0x00FF
	call	_zex_exit
	di
002$:
	halt
	jr	002$
