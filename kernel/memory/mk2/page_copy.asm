; -----------------------------------------------------------------------------
; KonaOS page copy: the Zephyr-80 Mk2 nucleus's one byte mover between
; physical pages.
;
; The nucleus runs in the system bank.  To reach a page it does not map
; (a program's page, an extended-memory object's, a ROM page) it remaps
; one of its two copy windows (system entries KOS_DAT_COPY_SRC_ENTRY and
; KOS_DAT_COPY_DST_ENTRY, memory.inc) and moves the bytes with one LDIR,
; directly between the two pages: nothing is staged in common memory.
; The windows belong to this routine; no interrupt path maps or reads
; them, so it runs under the caller's interrupt state, and the mirrors are
; written before each OUT (KOS_DAT_REMAP).
;
; Interface (kernel-internal, kernel/memory/mk2/pages.h):
;
;   struct kos_page_copy_req {
;       uint8_t  src_page;  a page 00h-7Fh: src is the offset in it;
;                           KOS_PAGE_DIRECT: src is a nucleus address;
;                           KOS_PAGE_ZEROES: the chunk is zero-filled
;       uint8_t  dst_page;  a page, or KOS_PAGE_DIRECT
;       uint16_t src;
;       uint16_t dst;
;       uint16_t length;    1-2000h, inside both pages (the caller chunks)
;   };
;   void kos_page_copy(const struct kos_page_copy_req *req);
;
; A nucleus address lies outside the copy windows (A000h-DFFFh).
; SDCC sdcccall(1): the request pointer arrives in HL.  Clobbers: AF, BC,
; DE, HL.
; -----------------------------------------------------------------------------

	.module page_copy_mk2
	.include "hardware.inc"
	.include "memory.inc"
	.include "map_ops.inc"

	.globl _kos_page_copy

KOS_PAGE_ZEROES = 0x80		; pages.h
KOS_PAGE_DIRECT = 0xFF

	.area _CODE

_kos_page_copy::
	ld	c,(hl)			; C = src_page
	inc	hl
	ld	b,(hl)			; B = dst_page
	inc	hl
	ld	e,(hl)
	inc	hl
	ld	d,(hl)			; DE = src
	inc	hl
	push	de
	ld	e,(hl)
	inc	hl
	ld	d,(hl)			; DE = dst
	inc	hl
	ld	a,(hl)
	inc	hl
	ld	h,(hl)
	ld	l,a
	push	hl			; stack: src, length
	ld	a,b
	cp	#KOS_PAGE_DIRECT
	jr	z,page_dst_ready
	KOS_DAT_REMAP KOS_DAT_COPY_DST_ENTRY
	ld	hl,#KOS_DAT_COPY_DST_BASE
	add	hl,de
	ex	de,hl			; DE = the destination in its window
page_dst_ready:
	ld	a,c
	cp	#KOS_PAGE_ZEROES
	jr	z,page_zero
	cp	#KOS_PAGE_DIRECT
	jr	z,page_src_ready
	KOS_DAT_REMAP KOS_DAT_COPY_SRC_ENTRY
	pop	bc			; BC = length
	pop	hl
	push	de
	ld	de,#KOS_DAT_COPY_SRC_BASE
	add	hl,de			; HL = the source in its window
	pop	de
	ldir
	ret
page_src_ready:
	pop	bc
	pop	hl
	ldir
	ret
page_zero:
	pop	bc
	pop	hl
	ld	h,d
	ld	l,e
	xor	a
	ld	(de),a
	dec	bc
	ld	a,b
	or	c
	ret	z
	inc	de
	ldir
	ret
