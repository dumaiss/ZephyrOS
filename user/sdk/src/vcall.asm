; -----------------------------------------------------------------------------
; KonaOS libkos: the veneer trampoline (architecture 7.2).
;
; A placed program calls across zones through veneers the placer
; generates (tools/exeplace.py), one per target, all resident:
;
;	__vn_<fn>::
;	call	kos_vcall
;	.db	<zone>		; 0 = the home zone, 1-4 = far code segment n
;	.dw	_<fn>		; the target, linked in its zone
;
; The caller made an ordinary call to the veneer, so its arguments are in
; registers (A, HL, DE, BC) and on the stack above its return address.
; The trampoline keeps both exactly: it saves the registers, moves the
; caller's return address and the current latch onto its frame stack,
; maps the target's zone, puts its own return address where the caller's
; was and enters the target with the registers restored.  The callee sees
; the stack its caller built.  On return the trampoline restores the
; latch and returns to the caller with the results (A, DE, HL, DEHL) and
; the flags intact.
;
; Mapping.  The home zone is the program's own bank: P = 0 in the latch
; (bits 7-5).  A far segment is its cached token (bcall.asm's cache: the
; first call of a segment spends one SYS_SEG_MAP).
;
; Reentrancy.  The register scratch is used only while entering and
; leaving; veneer calls nest through the frame stack.  Nothing reenters
; the trampoline from an event handler: the placer keeps every handler
; and all it reaches resident, so a handler never calls a veneer.
; -----------------------------------------------------------------------------

	.module kos_vcall

	.globl kos_vcall
	.globl _bcall_token
	.globl _aperture_open
	.globl _exe_runtime_error

KOS_MEMCTL_PORT = 0x00
KOS_RT_EFARMAP  = 0x30
KOS_RT_EVDEPTH  = 0x31
VC_DEPTH        = 16		; nested veneer calls
VC_FRAME        = 3		; the caller's return address, the latch

	.area _CODE

kos_vcall::
	ld	(vc_hl), hl
	ld	(vc_de), de
	ld	(vc_bc), bc
	ld	(vc_a), a
	pop	hl			; the veneer's inline zone and target
	ld	c, (hl)
	inc	hl
	ld	e, (hl)
	inc	hl
	ld	d, (hl)
	ld	(vc_target), de
	pop	de			; the caller's return address
	ld	hl, (vc_fp)
	ld	a, h
	or	l
	jr	nz, 1$
	ld	hl, #vc_frames		; the first call: an empty frame stack
1$:	push	de
	ld	de, #vc_frames + VC_DEPTH * VC_FRAME
	or	a
	sbc	hl, de
	add	hl, de
	pop	de
	jr	z, vc_deep
	ld	(hl), e
	inc	hl
	ld	(hl), d
	inc	hl
	push	hl
	ld	a, c
	or	a
	jr	nz, 2$
	in	a, (KOS_MEMCTL_PORT)	; the home zone: close the aperture
	ld	b, a
	and	#0x1F
	out	(KOS_MEMCTL_PORT), a
	ld	a, b			; A = the latch to restore
	jr	3$
2$:	call	_bcall_token		; C = segment id -> A = token, 0 if none
	or	a
	jr	z, vc_unmapped
	call	_aperture_open		; A = the latch to restore
3$:	pop	hl
	ld	(hl), a
	inc	hl
	ld	(vc_fp), hl
	ld	hl, #vc_return		; the callee returns here
	push	hl
	ld	hl, (vc_target)
	push	hl
	ld	hl, (vc_hl)
	ld	de, (vc_de)
	ld	bc, (vc_bc)
	ld	a, (vc_a)
	ret				; enter the target

vc_return:
	ld	(vc_hl), hl
	ld	(vc_de), de
	push	af
	ld	hl, (vc_fp)
	dec	hl
	ld	a, (hl)			; the caller's latch
	out	(KOS_MEMCTL_PORT), a
	dec	hl
	ld	d, (hl)
	dec	hl
	ld	e, (hl)			; the caller's return address
	ld	(vc_fp), hl
	pop	af
	push	de
	ld	hl, (vc_hl)
	ld	de, (vc_de)
	ret

; A target zone that cannot be mapped, or veneer calls nested deeper than
; the frame stack: the runtime error path (runtime.asm), as a banked
; call reports a refused segment.
; The caller's return address goes back on the stack, uncommitted to the
; frame stack, and the call returns -code in DE (and A).
vc_unmapped:
	pop	hl			; just past the frame's return address
	dec	hl
	ld	d, (hl)
	dec	hl
	ld	e, (hl)
	push	de
	ld	a, #KOS_RT_EFARMAP
	call	_exe_runtime_error
	ld	a, e
	ret
vc_deep:
	push	de			; DE = the caller's return address
	ld	a, #KOS_RT_EVDEPTH
	call	_exe_runtime_error
	ld	a, e
	ret

	.area _DATA

vc_hl:		.ds	2
vc_de:		.ds	2
vc_bc:		.ds	2
vc_a:		.ds	1
vc_target:	.ds	2
vc_fp:		.ds	2
vc_frames:	.ds	VC_DEPTH * VC_FRAME
