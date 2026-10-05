; -----------------------------------------------------------------------------
; fsdeep entry stub (M9 exec depth-probe program, test artifact).
;
; The launch gate enters here in mode 10 with HL = 0080h, the startup block
; v1.  The depth probe ignores its arguments (it is launched with none), so
; the stub ignores HL too.
; -----------------------------------------------------------------------------

	.module fsdeep_entry

	.globl dp_entry
	.globl _dp_main
	.globl _zex_exit

	.area _CODE

dp_entry::
	call	_dp_main
	ld	hl,#0x02FE
	call	_zex_exit
	di
001$:
	halt
	jr	001$
