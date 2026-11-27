; -----------------------------------------------------------------------------
; KonaOS page copy: the Zephyr-80 Mk2 nucleus's one byte mover between
; physical pages.
;
; The nucleus runs in the system bank.  To reach a page it does not map
; (a program's page, an extended-memory object's, a ROM page) it remaps
; one of its two copy windows (system entries KOS_DAT_COPY_SRC_ENTRY and
; KOS_DAT_COPY_DST_ENTRY, memory.inc) and moves the bytes with one KOS_FASTCOPY,
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
; A nucleus address lies outside the copy windows (A000h-DFFFh), and so
; does the caller's stack (kos_driver_copy below runs a driver's copy on
; its own).  SDCC sdcccall(1): the request pointer arrives in HL.  Clobbers: AF, BC,
; DE, HL.
;
; Page lists (pages.h): a "map" is a page list indexed by address >> 13 (a
; DAT image for a logical address, an object's pages for an object
; offset), or 0 for a nucleus address.  The list forms resolve each side
; per chunk and split the copy at every page edge on either side:
;
;   void kos_pages_copy(const uint8_t *src_map, uint16_t src,
;                       const uint8_t *dst_map, uint16_t dst,
;                       uint16_t length);
;   void kos_pages_zero(const uint8_t *dst_map, uint16_t dst,
;                       uint16_t length);
;   void kos_driver_copy(const uint8_t *src_map, uint16_t src,
;                        const uint8_t *dst_map, uint16_t dst,
;                        uint16_t length);
;
; kos_driver_copy is kos_pages_copy for a driver's call.  A driver may
; call from a private stack in its io block, which lies in the copy
; windows, so the copy moves to its own stack (fixed data) before the
; first remap and comes back after the windows show what they showed at
; the call (the domain), written mirror first.  The caller's stack keeps
; its return address and nothing below it.
;
; HL and DE carry the first two arguments, the rest are on the stack and
; the callee's to remove.  The caller has checked every range; a length of
; 0 moves nothing.  Their chunk state is fixed data: no interrupt path
; copies (the windows are this file's).  Clobbers: AF, BC, DE, HL.
; -----------------------------------------------------------------------------

	.module page_copy_mk2
	.include "hardware.inc"
	.include "memory.inc"
	.include "map_ops.inc"
	.include "kernel/runtime/fastcopy.inc"

	.globl _kos_page_copy
	.globl _kos_pages_copy
	.globl _kos_pages_zero
	.globl _kos_driver_copy
	.globl kos_driver_copy_stack, kos_driver_copy_stack_top

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
	jr	page_move
page_src_ready:
	pop	bc
	pop	hl
page_move:
	KOS_FASTCOPY
	ret
page_zero:
	pop	bc
	pop	hl
	ld	h,d
	ld	l,e
	xor	a
	ld	(de),a			; seed one byte, then copy it forward
	dec	bc
	ld	a,b
	or	c
	ret	z
	inc	de
	jr	page_move

; The page-list forms: one request (pages_req, struct kos_page_copy_req)
; per chunk, each side resolved through its map.
REQ_SRC_PAGE = 0
REQ_DST_PAGE = 1
REQ_SRC      = 2
REQ_DST      = 4
REQ_LENGTH   = 6

_kos_pages_zero::			; HL = dst_map, DE = dst, stack: length
	ld	(pages_dst_map),hl
	ld	(pages_dst),de
	pop	bc
	pop	hl			; HL = length
	push	bc
	ld	a,#KOS_PAGE_ZEROES
	ld	(pages_req + REQ_SRC_PAGE),a
	ld	a,#1
	jr	pages_start
_kos_pages_copy::			; HL = src_map, DE = src,
	ld	(pages_src_map),hl	; stack: dst_map, dst, length
	ld	(pages_src),de
	pop	bc
	pop	hl
	ld	(pages_dst_map),hl
	pop	hl
	ld	(pages_dst),hl
	pop	hl			; HL = length
	push	bc
	xor	a
pages_start:			; A = zero-fill, HL = length
	ld	(pages_zero),a
pages_loop:				; HL = the bytes left
	ld	a,h
	or	l
	ret	z
	ld	(pages_left),hl
	ld	(pages_req + REQ_LENGTH),hl
	ld	de,(pages_dst)
	ld	hl,(pages_dst_map)
	call	pages_side
	ld	(pages_req + REQ_DST_PAGE),a
	ld	(pages_req + REQ_DST),de
	call	pages_clamp
	ld	a,(pages_zero)
	or	a
	jr	nz,pages_move
	ld	de,(pages_src)
	ld	hl,(pages_src_map)
	call	pages_side
	ld	(pages_req + REQ_SRC_PAGE),a
	ld	(pages_req + REQ_SRC),de
	call	pages_clamp
pages_move:
	ld	hl,#pages_req
	call	_kos_page_copy
	ld	de,(pages_req + REQ_LENGTH)
	ld	hl,(pages_src)
	add	hl,de
	ld	(pages_src),hl
	ld	hl,(pages_dst)
	add	hl,de
	ld	(pages_dst),hl
	ld	hl,(pages_left)
	or	a
	sbc	hl,de
	jr	pages_loop

_kos_driver_copy::			; HL = src_map, DE = src,
	ld	(pages_src_map),hl	; stack: dst_map, dst, length
	ld	(pages_src),de
	pop	bc
	pop	hl
	ld	(pages_dst_map),hl
	pop	hl
	ld	(pages_dst),hl
	pop	hl			; HL = length
	push	bc
	ld	(driver_copy_sp),sp
	ld	sp,#kos_driver_copy_stack_top
	ld	bc,(KOS_DAT_SYSTEM + KOS_DAT_COPY_SRC_ENTRY)
	push	bc			; C, B = the windows' pages
	xor	a
	call	pages_start
	pop	bc
	ld	a,c
	KOS_DAT_SET KOS_DAT_COPY_SRC_ENTRY
	ld	a,b
	KOS_DAT_SET KOS_DAT_COPY_DST_ENTRY
	ld	sp,(driver_copy_sp)
	ret

; One side of a chunk: HL = its map (0: a nucleus address), DE = the
; address.  Returns A = the page (KOS_PAGE_DIRECT), DE = the offset in it
; (the address) and HL = the room left in that page (FFFFh).
pages_side:
	ld	a,h
	or	l
	jr	z,pages_direct
	ld	a,d
	rlca
	rlca
	rlca
	and	#7			; the logical page, address >> 13
	add	a,l
	ld	l,a
	adc	a,h
	sub	l
	ld	h,a
	ld	a,d
	and	#0x1F
	ld	d,a			; DE = the offset
	ld	a,(hl)			; A = the page
	ld	hl,#0x2000
	or	a
	sbc	hl,de
	ret
pages_direct:
	ld	a,#KOS_PAGE_DIRECT
	ld	hl,#0xFFFF
	ret

; The chunk's length, no more than HL (the room on one side).
pages_clamp:
	ld	de,(pages_req + REQ_LENGTH)
	or	a
	sbc	hl,de
	ret	nc
	add	hl,de
	ld	(pages_req + REQ_LENGTH),hl
	ret

	.area _DATA

pages_req:
	.ds	8
pages_src_map:
	.ds	2
pages_src:
	.ds	2
pages_dst_map:
	.ds	2
pages_dst:
	.ds	2
pages_left:
	.ds	2
pages_zero:
	.ds	1
driver_copy_sp:
	.ds	2
; The driver copy stack: the saved windows (2 bytes), the loop's return (2) and
; kos_page_copy's frame (6), then a maskable interrupt's stub push (4)
; and a real-time tick (4) on top.
kos_driver_copy_stack::
	.ds	18
kos_driver_copy_stack_top::
