; -----------------------------------------------------------------------------
; ZephyrOS M7 native monitor entry stub (user program, first _CODE object).
;
; The process launch gate enters here in mode 10 with interrupts enabled
; and HL = 0000h (ZEX v1 has no startup block); a nonzero HL is a launch
; contract violation and exits with a distinguishable status.
;
; mon_main() runs the monitor loop and exits only through SYS_EXIT (Q).
; The fallback below is a defensive exit if it ever returns.
; -----------------------------------------------------------------------------

	.module monitor_entry

	.globl mon_entry
	.globl _mon_main
	.globl _zex_exit

	.area _CODE

mon_entry::
	ld	a,h
	or	l
	jr	nz,mon_entry_bad_hl
	call	_mon_main
	; mon_main must exit via zex_exit; reaching here is a program bug.
	ld	hl,#0x0203		; MON_EXIT_INTERNAL
	call	_zex_exit
	di
001$:
	halt
	jr	001$

mon_entry_bad_hl:
	ld	hl,#0x0204		; MON_EXIT_LAUNCH
	call	_zex_exit
	di
002$:
	halt
	jr	002$
