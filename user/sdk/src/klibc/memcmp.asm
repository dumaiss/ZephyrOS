; int memcmp(const void *a, const void *b, unsigned int n);
; sdcccall(1): HL = a, DE = b, n on the stack (removed by the callee);
; result in DE: the difference of the first unequal bytes, 0 if none.
	.module kos_memcmp
	.globl _memcmp
	.area _CODE
_memcmp::
	push	hl
	ld	hl,#4
	add	hl,sp
	ld	c,(hl)
	inc	hl
	ld	b,(hl)			; BC = n
	pop	hl
memcmp_next:
	ld	a,b
	or	c
	jr	z,memcmp_equal
	ld	a,(de)
	cp	(hl)
	jr	nz,memcmp_differ
	inc	hl
	inc	de
	dec	bc
	jr	memcmp_next
memcmp_equal:
	ld	de,#0
	jr	memcmp_return
memcmp_differ:
	ld	c,a			; *b
	ld	a,(hl)			; *a
	sub	c
	ld	e,a
	sbc	a,a
	ld	d,a
memcmp_return:
	pop	hl			; the return address
	pop	bc			; drop n
	jp	(hl)
