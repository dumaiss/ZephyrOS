; int strncmp(const char *a, const char *b, unsigned int n);
; sdcccall(1): HL = a, DE = b, n on the stack (removed by the callee);
; result in DE as strcmp, 0 once n bytes compared equal.
	.module kos_strncmp
	.globl _strncmp
	.area _CODE
_strncmp::
	push	hl
	ld	hl,#4
	add	hl,sp
	ld	c,(hl)
	inc	hl
	ld	b,(hl)			; BC = n
	pop	hl
strncmp_next:
	ld	a,b
	or	c
	jr	z,strncmp_equal
	ld	a,(de)
	cp	(hl)
	jr	nz,strncmp_differ
	or	a
	jr	z,strncmp_equal
	inc	hl
	inc	de
	dec	bc
	jr	strncmp_next
strncmp_equal:
	ld	de,#0
	jr	strncmp_return
strncmp_differ:
	ld	c,a			; *b
	ld	a,(hl)			; *a
	sub	c
	ld	e,a
	sbc	a,a
	ld	d,a
strncmp_return:
	pop	hl			; the return address
	pop	bc			; drop n
	jp	(hl)
