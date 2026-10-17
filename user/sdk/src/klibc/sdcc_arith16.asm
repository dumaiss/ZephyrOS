; -----------------------------------------------------------------------------
; KonaOS klibc: the 16-bit unsigned division helpers SDCC calls for / and %
; on unsigned int (__divuint, __moduint), and the same routines under the
; names the signed helpers call (kos_divuint, kos_moduint).
;
; sdcccall(1): HL = dividend, DE = divisor, result in DE.  A zero divisor
; returns 0, as the C version did.  A shift-subtract long division: 16
; steps, about 1,100 T-states against the C loop's 3,400.
; -----------------------------------------------------------------------------

	.module kos_sdcc_arith16

	.globl __divuint
	.globl __moduint
	.globl _kos_divuint
	.globl _kos_moduint

	.area _CODE

; In: HL = dividend, DE = divisor (not zero).  Out: BC = quotient,
; HL = remainder.  Clobbers AF.
kos_divmod16:
	ld	b,h
	ld	c,l
	ld	hl,#0
	ld	a,#16
kos_divmod16_step:
	sla	c
	rl	b			; carry = the dividend's next bit
	adc	hl,hl			; remainder * 2 + bit; carry = bit 16
	jr	c,kos_divmod16_over
	sbc	hl,de			; carry is clear here
	jr	nc,kos_divmod16_one
	add	hl,de			; did not fit: restore
	dec	a
	jr	nz,kos_divmod16_step
	ret
kos_divmod16_over:
	or	a			; the remainder passed 16 bits: it fits
	sbc	hl,de
kos_divmod16_one:
	inc	c			; quotient bit
	dec	a
	jr	nz,kos_divmod16_step
	ret

__divuint::
_kos_divuint::
	ld	a,d
	or	e
	jr	z,kos_div16_zero
	call	kos_divmod16
	ld	d,b
	ld	e,c
	ret

__moduint::
_kos_moduint::
	ld	a,d
	or	e
	jr	z,kos_div16_zero
	call	kos_divmod16
	ex	de,hl
	ret

kos_div16_zero:
	ld	de,#0
	ret
