; -----------------------------------------------------------------------------
; KonaOS klibc: divide an unsigned integer in place by a small base and
; return the remainder - one step of digit conversion, quotient and
; remainder together (utoa, ultoa, itoa, ltoa).
;
;   unsigned int kos_divmod16(unsigned int *value, unsigned int base);
;   unsigned int kos_divmod32(unsigned long *value, unsigned int base);
;
; sdcccall(1): HL = value pointer, DE = base (2..255), remainder in DE.
; Long division a byte at a time from the most significant byte: the
; running remainder stays below the base, so each step is a 16/8 division
; with an 8-bit quotient (8 shift-subtract steps).
; -----------------------------------------------------------------------------

	.module kos_divmod

	.globl _kos_divmod16
	.globl _kos_divmod32

	.area _CODE

_kos_divmod16::
	inc	hl			; the most significant byte
	ld	b,#2
	jr	kos_divmod_bytes
_kos_divmod32::
	inc	hl
	inc	hl
	inc	hl
	ld	b,#4
kos_divmod_bytes:
	ld	c,e			; C = base
	xor	a			; A = remainder
kos_divmod_byte:
	; A:(HL) / C -> quotient byte in (HL), remainder in A.
	ld	e,(hl)
	ld	d,#8
kos_divmod_bit:
	sla	e
	rla				; remainder * 2 + bit; carry = bit 8
	jr	c,kos_divmod_fit
	cp	c
	jr	c,kos_divmod_next
kos_divmod_fit:
	sub	c
	inc	e			; quotient bit
kos_divmod_next:
	dec	d
	jr	nz,kos_divmod_bit
	ld	(hl),e
	dec	hl
	djnz	kos_divmod_byte
	ld	e,a
	ld	d,#0
	ret
