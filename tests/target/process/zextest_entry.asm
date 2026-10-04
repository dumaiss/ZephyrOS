; -----------------------------------------------------------------------------
; zextest entry stub (M6 acceptance program, test artifact).
;
; The first _CODE object of the zextest link, so the C symbol lands at
; 0100h and the packed header carries entry_offset 0.  The process launch
; gate enters here in mode 10 with interrupts enabled and HL = 0000h
; (ZEX v1 has no startup block); a nonzero HL is a launch contract
; violation and exits with a distinguishable failure status.
;
; zextest's main never returns; the fallback below is a defensive exit if
; it ever does.
; -----------------------------------------------------------------------------

	.module zextest_entry

	.globl zt_entry
	.globl _zt_main
	.globl _zt_get_sp
	.globl _zex_exit

	.area _CODE

zt_entry::
	ld	a,h
	or	l
	jr	nz,zt_entry_bad_hl
	call	_zt_main
	; _zt_main must exit via zex_exit; reaching here is a test bug.
	ld	hl,#0x01FE
	call	_zex_exit
	di
001$:
	halt
	jr	001$

zt_entry_bad_hl:
	; ZOS_PCST_ZEXTEST_FAIL | 0x01 (see tests/target/include/process_selftest.h)
	ld	hl,#0x0101
	call	_zex_exit
	di
002$:
	halt
	jr	002$

; unsigned int zt_get_sp(void);  SDCC returns the 16-bit value in DE.
_zt_get_sp::
	ld	hl,#0x0000
	add	hl,sp
	ex	de,hl
	ret
