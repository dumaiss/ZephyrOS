; -----------------------------------------------------------------------------
; KOS_FASTCOPY: copy BC bytes from (HL) to (DE), ascending, as LDIR does.
;
; LDIR costs 21 T-states a byte because it repeats one instruction.  Here
; the BC mod 16 remainder goes byte by byte, then the rest in runs of
; sixteen LDIs with one JP PE (P/V is set while BC is not zero): 16.6
; T-states a byte.  BC = 0 copies nothing (LDIR would copy 64 KiB).  On
; exit HL and DE are past the copy and BC is 0, exactly as after LDIR;
; A and F are clobbered.  Interruptible between any two instructions, like
; LDIR.  No call and no stack use: it expands in place, so it can run on
; a stack with no room for a return address.  The reusable labels
; 9001$-9003$ must be unused in the expanding scope.
; -----------------------------------------------------------------------------

	.macro	KOS_FASTCOPY
	ld	a,b
	or	c
	jr	z,9003$
	ld	a,c
	and	#0x0F
	jr	z,9002$
9001$:
	ldi
	dec	a
	jr	nz,9001$
	ld	a,b
	or	c
	jr	z,9003$
9002$:
	ldi
	ldi
	ldi
	ldi
	ldi
	ldi
	ldi
	ldi
	ldi
	ldi
	ldi
	ldi
	ldi
	ldi
	ldi
	ldi
	jp	pe,9002$
9003$:
	.endm
