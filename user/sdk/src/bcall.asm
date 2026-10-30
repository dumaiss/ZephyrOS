; -----------------------------------------------------------------------------
; KonaOS libkos: the banked-call trampoline.
;
; A program marks a far function `__banked` and calls it like any other
; function.  SDCC then emits
;
;	ld	e, #b_<fn>	; the segment id the definition declared
;	ld	hl, #_<fn>	; the far entry address (linked at 8000h)
;	call	___sdcc_bcall_ehl
;
; with every argument on the stack (the z80 banked-call convention: all
; parameters are passed on the stack and the caller clears them up).
; This member is the trampoline SDCC's calling convention names: it maps
; the far segment, opens the mode-10 aperture onto it, calls the entry
; and restores the caller's latch.  `farcall` (seg.c) stays as the
; primitive that maps an explicit segment and function pointer.
;
; Stack shape.  A `__banked` definition reads its arguments at fixed
; offsets from its entry stack pointer: SP+0..1 is the address its RET
; pops, SP+2 and SP+3..4 are reserved, and the first argument starts at
; SP+5.  The caller puts the arguments above the return address, so the
; trampoline must spend exactly three bytes between its entry and the
; transfer: the one-byte slot holds the aperture latch to restore and the
; two-byte slot is the return address of the jump.  Nothing else may stay
; on the stack while the far code runs, and the transfer is a plain
; CALL/JUMP: the aperture is already open, so no syscall sits on the far
; path.
;
; Registers.  The caller spills everything it needs across a banked call;
; the only registers that carry a result are A (8 bits), DE (16 bits) and
; DEHL (32 bits).  Before the transfer the trampoline may use any
; register; after it returns, only A, B and C are touched - D, E, H and L
; are the result - and A comes back through the saved flags.
;
; Token cache.  A program has at most four far code segments, ids 1-4
; (`FAR1=`..`FAR4=`), and a banked call can only name one of them, so one
; token slot per id is exact: the first call of a segment spends one
; SYS_SEG_MAP and every later call is the cached token.  A call naming
; anything else - a DATA segment (ids start at 5), an id the container
; does not have - finds no slot and takes the runtime-error path, as does
; a failed SYS_SEG_MAP.  The cache lives in this member's own BSS, so it
; is per-program state and the SDK's fixed low-data area does not grow.
; -----------------------------------------------------------------------------

	.module kos_bcall

	.globl ___sdcc_bcall_ehl
	.globl _exe_runtime_error
	.globl _aperture_open
	.globl _aperture_close
	.globl _segmap
	.globl _bcall_token

	.area _CODE

; The runtime code the trampoline reports: the program carried a banked
; call whose segment could not be mapped.  Deferred to the shared
; carrying error mapper (runtime.asm), which returns DE = -code.
KOS_RT_EFARMAP = 0x30

; ___sdcc_bcall_ehl: E = segment id, HL = far entry.
___sdcc_bcall_ehl::
	push	hl		; the entry address survives the mapping
	ld	c, e		; C = the segment id
	call	_bcall_token	; A = token, 0 when the id is not far code
	pop	hl
	or	a
	jr	z, _bcall_rt
	call	_aperture_open	; A = token -> A = the previous latch
	push	af
	inc	sp		; the one-byte slot: the latch to restore
	call	_bcall_jump
	dec	sp
	pop	bc		; B = the latch saved above
	push	af		; keep the call's result (A and the flags)
	ld	a, b
	call	_aperture_close	; restore the caller's mapping
	pop	af
	ret

; The refused mapping: report it and return -KOS_RT_EFARMAP in DE (and in
; A too, so a banked function that returns 8 bits reads the same code).
_bcall_rt:
	ld	a, #KOS_RT_EFARMAP
	call	_exe_runtime_error
	ld	a, e
	ret

; The transfer: the far entry runs with the two-byte slot holding this
; call's return address, so the definition's RET lands in the trampoline.
_bcall_jump:
	jp	(hl)

; A = the aperture token of the far segment in C, or 0 when C is not one
; of the program's far code segments (ids 1-4) or the map was refused.  A
; hit costs no syscall: the token is cached on the first call.
_bcall_token::
	ld	a, c
	dec	a		; the ids are 1-4
	cp	#4
	jr	nc, 2$
	ld	hl, #_bcall_cache
	add	a, l
	ld	l, a
	jr	nc, 1$		; the slot offset must not carry into H
	inc	h
1$:
	ld	a, (hl)		; the cached token, 0 when the slot is unused
	or	a		; LD leaves the flags alone
	ret	nz		; a hit
	push	hl		; the entry the token belongs in
	ld	l, c
	ld	h, #0
	call	_segmap		; DE = token, negative on refusal
	pop	hl
	bit	7, d
	jr	nz, 2$
	ld	(hl), e
	ld	a, e
	ret

2$:	xor	a		; not a far segment, or the map failed
	ret

; The token cache: one token per far segment id, zero-filled at load and
; empty while it is 0 (a token is at least 1).
	.area _DATA

_bcall_cache::
	.ds	4
