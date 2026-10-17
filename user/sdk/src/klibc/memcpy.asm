; void *memcpy(void *dst, const void *src, unsigned int n);
; sdcccall(1): HL = dst, DE = src, n on the stack (removed by the callee);
; returns dst in DE.  An unrolled LDI run: 16.6 T-states a byte.
	.module kos_memcpy
	.globl _memcpy
	.area _CODE
_memcpy::
	push	hl			; dst, for the return value
	ld	hl,#4
	add	hl,sp
	ld	c,(hl)
	inc	hl
	ld	b,(hl)			; BC = n
	pop	hl
	push	hl
	ex	de,hl			; HL = src, DE = dst
	ld	a,b
	or	c
	jr	z,memcpy_done
	ld	a,c
	and	#0x0F
	jr	z,memcpy_16
memcpy_1:
	ldi
	dec	a
	jr	nz,memcpy_1
	ld	a,b
	or	c
	jr	z,memcpy_done
memcpy_16:
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
	jp	pe,memcpy_16
memcpy_done:
	pop	de			; dst
	pop	hl			; the return address
	pop	bc			; drop n
	jp	(hl)
