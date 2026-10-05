; -----------------------------------------------------------------------------
; fschild entry stub (M9 exec acceptance child, test artifact).
;
; First _CODE object of the fschild link, so the C entry lands at 0100h and
; the packed header carries entry_offset 0.  The launch gate enters here in
; mode 10 with HL = 0080h, the child's startup block v1.  The stub saves HL
; through the runtime glue (zos_args_init) before C code can clobber it, so
; ch_main() can reach the arguments with zos_args()/zos_split().
; -----------------------------------------------------------------------------

	.module fschild_entry

	.globl ch_entry
	.globl _ch_main
	.globl _zos_args_init
	.globl _zex_exit

	.area _CODE

ch_entry::
	call	_zos_args_init
	call	_ch_main
	; _ch_main must exit via zex_exit; reaching here is a test bug.
	ld	hl,#0x01FE
	call	_zex_exit
	di
001$:
	halt
	jr	001$
