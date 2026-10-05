; -----------------------------------------------------------------------------
; fsraw entry stub (M9 exec raw-return child, test artifact).
;
; The entry returns immediately without calling SYS_EXIT.  The common return
; trap therefore tears the child down with ZOS_EFAULT, which SYS_EXEC hands
; back to the parent as the child's exit status.
; -----------------------------------------------------------------------------

	.module fsraw_entry

	.globl rw_entry
	.globl _rw_main

	.area _CODE

rw_entry::
	call	_rw_main
	ret			; raw entry return -> ZOS_EFAULT
