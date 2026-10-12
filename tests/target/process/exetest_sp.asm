; -----------------------------------------------------------------------------
; exetest stack-pointer probe (test artifact, package 28c).
;
; A EXE program cannot read SP from C, and programs no longer carry entry
; assembly, so this single helper stays beside the test it serves.
; -----------------------------------------------------------------------------

	.module exetest_sp

	.globl _zt_get_sp

	.area _CODE

; unsigned int zt_get_sp(void);  SDCC returns the 16-bit value in DE.
_zt_get_sp::
	ld	hl,#0x0000
	add	hl,sp
	ex	de,hl
	ret
