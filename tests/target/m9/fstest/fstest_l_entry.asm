; -----------------------------------------------------------------------------
; fstest_l entry stub (M9 launcher-cleanup check, test artifact).
;
; A second native program exec'd after fstest.zex to prove the descriptor
; pool and CWD were released on exit.  The launch gate enters here in mode
; 10 with HL = 0080h, the startup block v1; this program has no arguments
; and ignores HL.
; -----------------------------------------------------------------------------

	.module fstest_l_entry

	.globl ftl_entry
	.globl _ftl_main
	.globl _zex_exit

	.area _CODE

ftl_entry::
	call	_ftl_main
	ld	hl,#0x00FE
	call	_zex_exit
	di
001$:
	halt
	jr	001$
