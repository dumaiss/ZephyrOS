; int strcmp(const char *a, const char *b);  HL = a, DE = b, result in DE:
; the difference of the first unequal bytes as unsigned chars, 0 if equal.
	.module kos_strcmp
	.globl _strcmp
	.area _CODE
_strcmp::
	ld	a,(de)
	cp	(hl)
	jr	nz,strcmp_differ
	or	a
	jr	z,strcmp_equal
	inc	hl
	inc	de
	jr	_strcmp
strcmp_equal:
	ld	d,a
	ld	e,a
	ret
strcmp_differ:
	ld	c,a			; *b
	ld	a,(hl)			; *a
	sub	c
	ld	e,a
	sbc	a,a			; borrow: *a < *b, the high byte is FFh
	ld	d,a
	ret
