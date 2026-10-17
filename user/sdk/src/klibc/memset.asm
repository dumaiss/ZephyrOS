; void *memset(void *dst, int c, unsigned int n);
; sdcccall(1): HL = dst, DE = c, n on the stack (removed by the callee);
; returns dst in DE.  The first byte is stored and spread with LDIR.
	.module kos_memset
	.globl _memset
	.area _CODE
_memset::
	push	hl			; dst
	ld	hl,#4
	add	hl,sp
	ld	c,(hl)
	inc	hl
	ld	b,(hl)			; BC = n
	pop	hl
	push	hl
	ld	a,b
	or	c
	jr	z,memset_done
	ld	(hl),e
	dec	bc
	ld	a,b
	or	c
	jr	z,memset_done
	ld	d,h
	ld	e,l
	inc	de
	ldir
memset_done:
	pop	de			; dst
	pop	hl			; the return address
	pop	bc			; drop n
	jp	(hl)
