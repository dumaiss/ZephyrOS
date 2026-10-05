; -----------------------------------------------------------------------------
; fstest entry stub (M9 filesystem syscall acceptance, test artifact).
;
; First _CODE object of the fstest link, so the C entry lands at 0100h and
; the packed header carries entry_offset 0.  The process launch gate enters
; here in mode 10 with interrupts enabled and HL = 0080h, the startup block
; v1.  The stub saves HL through the runtime glue (zos_args_init) so
; ft_main() can check the kernel-launched block (version 1, empty tail).
; -----------------------------------------------------------------------------

	.module fstest_entry

	.globl ft_entry
	.globl _ft_main
	.globl _zos_args_init
	.globl _zex_exit

	.area _CODE

ft_entry::
	call	_zos_args_init
	call	_ft_main
	; _ft_main exits via zex_exit; reaching here is a test bug.
	ld	hl,#0x00FE
	call	_zex_exit
	di
001$:
	halt
	jr	001$
