; void *memmove(void *dst, const void *src, unsigned int n);
; sdcccall(1): HL = dst, DE = src, n on the stack (removed by the callee);
; returns dst in DE.  Ascending (LDIR) unless dst lies inside the source
; above it, then descending (LDDR) from the ends.
	.module kos_memmove
	.globl _memmove
	.area _CODE
_memmove::
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
	jr	z,memmove_done
	or	a
	sbc	hl,de			; dst - src
	jr	z,memmove_done		; the same buffer
	jr	c,memmove_up		; dst below src: ascending is safe
	or	a
	sbc	hl,bc			; dst - src - n
	jr	nc,memmove_up		; dst at or past src + n: no overlap
	; Overlap with dst above src: copy from the ends down.
	pop	hl
	push	hl
	add	hl,bc
	dec	hl			; HL = dst + n - 1
	ex	de,hl			; DE = dst end, HL = src
	add	hl,bc
	dec	hl			; HL = src + n - 1
	lddr
	jr	memmove_done
memmove_up:
	pop	hl
	push	hl
	ex	de,hl			; HL = src, DE = dst
	ldir
memmove_done:
	pop	de			; dst
	pop	hl			; the return address
	pop	bc			; drop n
	jp	(hl)
