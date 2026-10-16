; -----------------------------------------------------------------------------
; Module-local SDCC runtime glue, shared by every MOD that needs it.
;
; A MOD is linked at base zero and may not import kernel symbols, so the
; freestanding SDCC runtime helpers the z80 code generator emits
; (___sdcc_call_iy / ___sdcc_call_hl, plus the ___sdcc_enter_ix frame
; helper selected by --opt-code-size) must be provided inside each module
; image.  The kernel's own copy (kernel/runtime/sdcc_helpers.asm) is
; deliberately NOT referenced: importing it would be a forbidden external
; reference, while these intra-module calls are made position-independent
; by KREL_ABS16 relocation once the loader has chosen an arena address.
;
; This one source is compiled to a relocatable object and packed into every
; MOD that needs it (the SIO module variants and the synthetic test
; fixtures); each packed container carries its own copy.
; -----------------------------------------------------------------------------

	.module sdcc_indirect_calls
	.globl ___sdcc_call_iy, ___sdcc_call_hl, ___sdcc_enter_ix
	.globl _kos_block_copy

	.area _CODE

___sdcc_call_iy::
	jp	(iy)

___sdcc_call_hl::
	jp	(hl)

; Canonical sdcc z80 crtenter.s frame-entry helper (see kernel copy).
___sdcc_enter_ix::
	pop	hl
	push	ix
	ld	ix, #0
	add	ix, sp
	jp	(hl)

; ---------------------------------------------------------------------------
; void kos_block_copy(void *dst, const void *src, uint16_t count);
;
; The drivers' block copy: an unrolled LDI run, 16.6 T-states a byte (LDIR
; costs 21, and a C byte loop compiles to several hundred).  sdcccall(1): HL = dst, DE = src,
; count on the stack, which the callee removes.  A count of 0 copies
; nothing.  Clobbers AF, BC, DE, HL (IX and IY are untouched).
; ---------------------------------------------------------------------------
_kos_block_copy::
	push	hl			; dst
	ld	hl,#4
	add	hl,sp			; HL -> count (above dst and the return)
	ld	c,(hl)
	inc	hl
	ld	b,(hl)
	pop	hl
	ex	de,hl			; HL = src, DE = dst
	; BC mod 16 byte by byte, then sixteen LDIs per JP PE (P/V is set
	; while BC is not zero).
	ld	a,b
	or	c
	jr	z,kos_block_copy_done
	ld	a,c
	and	#0x0F
	jr	z,kos_block_copy_16
kos_block_copy_1:
	ldi
	dec	a
	jr	nz,kos_block_copy_1
	ld	a,b
	or	c
	jr	z,kos_block_copy_done
kos_block_copy_16:
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
	jp	pe,kos_block_copy_16
kos_block_copy_done:
	pop	hl			; the return address
	pop	bc			; drop count
	jp	(hl)
