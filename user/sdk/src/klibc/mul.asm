; -----------------------------------------------------------------------------
; KonaOS klibc: the multiplication helpers SDCC calls for the Z80.
;
;   __mulint    int x int (also unsigned): HL x DE, low 16 bits in DE
;   __mulschar  signed char x signed char -> int:     A x L, result in DE
;   __mulsuchar unsigned char A x signed char L -> int, result in DE
;   __muluschar signed char A x unsigned char L -> int, result in DE
;
; sdcccall(1), the conventions of SDCC's own library (unsigned char x
; unsigned char is generated inline).  One archive member: a program
; links it only when it multiplies.  Shift-add from the multiplier's top
; bit, 8 steps when it fits a byte, 16 otherwise.
; -----------------------------------------------------------------------------

	.module kos_mul

	.globl __mulint
	.globl __mulschar
	.globl __mulsuchar
	.globl __muluschar

	.area _CODE

__mulint::
	ld	b,h
	ld	c,l			; BC = multiplier, DE = multiplicand
; BC x DE -> DE (low 16 bits).  Clobbers AF, BC, HL.
kos_mul16:
	ld	hl,#0
	ld	a,b
	or	a
	jr	z,kos_mul_byte
	ld	a,#16
kos_mul_word:
	add	hl,hl
	sla	c
	rl	b
	jr	nc,kos_mul_word_skip
	add	hl,de
kos_mul_word_skip:
	dec	a
	jr	nz,kos_mul_word
	ex	de,hl
	ret
kos_mul_byte:
	ld	b,#8
kos_mul_byte_step:
	add	hl,hl
	sla	c
	jr	nc,kos_mul_byte_skip
	add	hl,de
kos_mul_byte_skip:
	djnz	kos_mul_byte_step
	ex	de,hl
	ret

__mulschar::
	ld	c,a			; BC = A, sign-extended
	rla
	sbc	a,a
	ld	b,a
	ld	e,l			; DE = L, sign-extended
	ld	a,l
	rla
	sbc	a,a
	ld	d,a
	jr	kos_mul16

__mulsuchar::
	ld	c,a			; BC = A, zero-extended
	ld	b,#0
	ld	e,l			; DE = L, sign-extended
	ld	a,l
	rla
	sbc	a,a
	ld	d,a
	jr	kos_mul16

__muluschar::
	ld	e,a			; DE = A, sign-extended
	rla
	sbc	a,a
	ld	d,a
	ld	c,l			; BC = L, zero-extended
	ld	b,#0
	jr	kos_mul16
