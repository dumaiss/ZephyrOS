; -----------------------------------------------------------------------------
; The literal-row drawing primitive.
;
; int tui_put_text(uint8_t x, uint8_t y, const char *text, uint8_t length,
;                  uint8_t attr)
;   sdcccall(1): A = x, L = y; text (2 bytes), length and attr on the stack.
;
; One window-relative row of literal glyph bytes with one attribute.  The
; effect is exactly `length` calls to tui_priv_set_cell(x + i, y, text[i],
; attr) with the same chars, attrs, dirty and invert bits and the same
; model_only and pending-row bookkeeping; cells past the window's right
; edge are clipped.
;
; The walk keeps its state in registers.  Main set: B the cells left, C
; the attribute in both nibbles, DE the text, HL the character cell, IX
; the attribute byte.  A cell's nibble is compared and merged with
; ((byte ^ C) & mask) ^ C.  Alternate set: B' the cell's dirty/invert bit
; (zero under model_only, which turns every bitmap update into a no-op
; and leaves the pending flag clear), HL' its dirty byte (a full 16-bit
; pointer: the bitmaps exceed 256 bytes), DE' the distance from the dirty
; map to the invert map, C' nonzero once a cell changed outside
; model_only.  The two nibble parities are two loop bodies.  IX is saved;
; the interrupt entry saves the alternate set.
; -----------------------------------------------------------------------------

	.module tui_text
	.globl _tui_put_text
	.globl _tui_state
	.globl _tui_chars
	.globl _tui_attrs
	.globl _tui_dirty
	.globl _tui_invert
	.globl _tui_row_pending

; Stable private tui_state offsets.  stride is last so existing offsets
; stay unchanged.
TUI_INITIALIZED       = 0
TUI_WINDOW_X          = 3
TUI_WINDOW_Y          = 4
TUI_WINDOW_W          = 5
TUI_WINDOW_H          = 6
TUI_MODEL_ONLY        = 37
TUI_STRIDE            = 38

	.area _DATA
tui_text_x:      .ds 1 ; window-relative x
tui_text_y:      .ds 1 ; window-relative y
tui_text_row:    .ds 1 ; the absolute row (window_y + y)
tui_text_remain: .ds 1 ; in-window cells
tui_text_attr:   .ds 1 ; the attribute in both nibbles

	.area _CODE

_tui_put_text::
	ld	(tui_text_x),a
	ld	a,l
	ld	(tui_text_y),a
	ld	a,(_tui_state+TUI_INITIALIZED)
	or	a
	jp	z,tui_text_einval

	; text = (SP+2..SP+3); a null pointer is refused before any work.
	ld	hl,#2
	add	hl,sp
	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	ld	a,e
	or	d
	jp	z,tui_text_einval

	; length = (SP+4), attr = (SP+5).
	inc	hl
	ld	a,(hl)
	inc	hl
	ld	b,(hl)
	or	a
	jp	z,tui_text_return_ok
	ld	c,a			; C = length
	ld	a,b
	and	#0x0f
	ld	b,a
	rlca
	rlca
	rlca
	rlca
	or	b
	ld	(tui_text_attr),a

	; Clip: y inside the window, then length to window width - x.
	ld	a,(tui_text_y)
	ld	b,a
	ld	a,(_tui_state+TUI_WINDOW_H)
	cp	b
	jp	c,tui_text_return_ok
	jp	z,tui_text_return_ok
	ld	a,(tui_text_x)
	ld	b,a
	ld	a,(_tui_state+TUI_WINDOW_W)
	sub	b
	jp	c,tui_text_return_ok
	jp	z,tui_text_return_ok
	cp	c
	jr	c,tui_text_len_ok
	ld	a,c
tui_text_len_ok:
	ld	(tui_text_remain),a
	push	de			; the text pointer

	; DE = the first cell's index: (window_y + y) x stride + window_x + x.
	ld	a,(tui_text_y)
	ld	b,a
	ld	a,(_tui_state+TUI_WINDOW_Y)
	add	a,b
	ld	(tui_text_row),a
	ld	b,a			; B = row (shift-and-add multiply)
	ld	a,(_tui_state+TUI_STRIDE)
	ld	e,a
	ld	d,#0
	ld	hl,#0
	ld	a,b
	ld	b,#8
tui_text_mul:
	add	hl,hl
	rlca
	jr	nc,tui_text_mul_next
	add	hl,de
tui_text_mul_next:
	djnz	tui_text_mul
	ex	de,hl			; DE = row x stride
	ld	a,(_tui_state+TUI_WINDOW_X)
	ld	b,a
	ld	a,(tui_text_x)
	add	a,b			; window x + x fits one byte
	add	a,e
	ld	e,a
	ld	a,d
	adc	a,#0
	ld	d,a			; DE = index

	push	ix
	push	de			; the index

	; Alternate set: B' = 1 << (index & 7), HL' = &dirty[index >> 3],
	; DE' = invert - dirty, C' = 0.
	ld	h,d
	ld	l,e
	srl	h
	rr	l
	srl	h
	rr	l
	srl	h
	rr	l
	ld	bc,#_tui_dirty
	add	hl,bc
	push	hl			; &dirty[index >> 3]
	ld	hl,#_tui_invert
	or	a
	sbc	hl,bc
	push	hl			; invert - dirty
	ld	a,e
	and	#0x07
	ld	b,a
	ld	a,#0x01
	jr	z,tui_text_mask_done
tui_text_mask_loop:
	add	a,a
	djnz	tui_text_mask_loop
tui_text_mask_done:
	ld	b,a
	ld	a,(_tui_state+TUI_MODEL_ONLY)
	or	a
	ld	a,b
	jr	z,tui_text_mask_live
	xor	a			; model_only: no bitmap or pending update
tui_text_mask_live:
	exx
	ld	b,a
	pop	de
	pop	hl
	ld	c,#0
	exx
	pop	de			; DE = the index

	; Main set: IX = &attrs[index >> 1], HL = &chars[index], B = cells,
	; C = the attribute in both nibbles.
	ld	h,d
	ld	l,e
	srl	h
	rr	l
	ld	bc,#_tui_attrs
	add	hl,bc
	push	hl
	pop	ix
	ld	hl,#_tui_chars
	add	hl,de
	ld	a,(tui_text_remain)
	ld	b,a
	ld	a,(tui_text_attr)
	ld	c,a
	ld	a,e			; the first cell's parity
	; The stack holds ix, then the text pointer: DE = text.
	push	hl
	ld	hl,#4
	add	hl,sp
	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	pop	hl
	rrca
	jp	c,tui_text_odd

; --- even cell: the attribute's low nibble ---------------------------------
; The whole attribute byte is tested first: when it already holds the
; attribute in both nibbles (unchanged text), the pair's odd cell needs
; only its character compared.
tui_text_even:
	ld	a,0(ix)
	cp	c
	jr	nz,tui_text_even_nib
	ld	a,(de)
	cp	(hl)
	jp	nz,tui_text_evq_mod
tui_text_evq_next:
	inc	de
	inc	hl
	exx
	rlc	b
	jr	nc,tui_text_evq_bit
	inc	hl
tui_text_evq_bit:
	exx
	dec	b
	jp	z,tui_text_done
	ld	a,(de)			; the odd cell: its nibble is equal
	cp	(hl)
	jp	nz,tui_text_odd_mod
	jp	tui_text_odd_next

tui_text_even_nib:
	ld	a,(de)
	cp	(hl)
	jp	nz,tui_text_even_mod
	ld	a,0(ix)
	xor	c
	and	#0x0f
	jp	nz,tui_text_even_mod
tui_text_even_next:
	inc	de
	inc	hl
	exx
	rlc	b
	jr	nc,tui_text_even_bit
	inc	hl
tui_text_even_bit:
	exx
	dec	b
	jp	z,tui_text_done

; --- odd cell: the attribute's high nibble ---------------------------------
tui_text_odd:
	ld	a,(de)
	cp	(hl)
	jp	nz,tui_text_odd_mod
	ld	a,0(ix)
	xor	c
	and	#0xf0
	jp	nz,tui_text_odd_mod
tui_text_odd_next:
	inc	de
	inc	hl
	inc	ix
	exx
	rlc	b
	jr	nc,tui_text_odd_bit
	inc	hl
tui_text_odd_bit:
	exx
	djnz	tui_text_even
	jp	tui_text_done

; A changed cell: tui_priv_set_cell's dirty/invert rules, then the store.
; Z on entry to the bookkeeping: the character is unchanged (an
; attribute-only change toggles invert); otherwise a clean cell gets its
; dirty bit and loses its invert bit.  A dirty cell only stores.
tui_text_even_mod:
	ld	a,(de)
	cp	(hl)
	exx
	ld	c,b			; changed (zero under model_only)
	ld	a,(hl)
	jr	z,tui_text_even_attr
	and	b
	jr	nz,tui_text_even_flagged
	ld	a,(hl)
	or	b
	ld	(hl),a
	push	hl
	add	hl,de
	ld	a,b
	cpl
	and	(hl)
	ld	(hl),a
	pop	hl
	jr	tui_text_even_flagged
tui_text_even_attr:
	and	b
	jr	nz,tui_text_even_flagged
	push	hl
	add	hl,de
	ld	a,(hl)
	xor	b
	ld	(hl),a
	pop	hl
tui_text_even_flagged:
	exx
	ld	a,(de)
	ld	(hl),a
	ld	a,0(ix)
	xor	c
	and	#0xf0
	xor	c
	ld	0(ix),a
	jp	tui_text_even_next

tui_text_evq_mod:
	ld	a,(de)
	cp	(hl)
	exx
	ld	c,b			; changed (zero under model_only)
	ld	a,(hl)
	jr	z,tui_text_evq_attr
	and	b
	jr	nz,tui_text_evq_flagged
	ld	a,(hl)
	or	b
	ld	(hl),a
	push	hl
	add	hl,de
	ld	a,b
	cpl
	and	(hl)
	ld	(hl),a
	pop	hl
	jr	tui_text_evq_flagged
tui_text_evq_attr:
	and	b
	jr	nz,tui_text_evq_flagged
	push	hl
	add	hl,de
	ld	a,(hl)
	xor	b
	ld	(hl),a
	pop	hl
tui_text_evq_flagged:
	exx
	ld	a,(de)
	ld	(hl),a
	ld	a,0(ix)
	xor	c
	and	#0xf0
	xor	c
	ld	0(ix),a
	jp	tui_text_evq_next

tui_text_odd_mod:
	ld	a,(de)
	cp	(hl)
	exx
	ld	c,b			; changed (zero under model_only)
	ld	a,(hl)
	jr	z,tui_text_odd_attr
	and	b
	jr	nz,tui_text_odd_flagged
	ld	a,(hl)
	or	b
	ld	(hl),a
	push	hl
	add	hl,de
	ld	a,b
	cpl
	and	(hl)
	ld	(hl),a
	pop	hl
	jr	tui_text_odd_flagged
tui_text_odd_attr:
	and	b
	jr	nz,tui_text_odd_flagged
	push	hl
	add	hl,de
	ld	a,(hl)
	xor	b
	ld	(hl),a
	pop	hl
tui_text_odd_flagged:
	exx
	ld	a,(de)
	ld	(hl),a
	ld	a,0(ix)
	xor	c
	and	#0x0f
	xor	c
	ld	0(ix),a
	jp	tui_text_odd_next

tui_text_done:
	; The row's pending flag, once, when a cell changed outside
	; model_only.
	exx
	ld	a,c
	exx
	or	a
	jr	z,tui_text_restore
	ld	a,(tui_text_row)
	ld	l,a
	ld	h,#0
	ld	de,#_tui_row_pending
	add	hl,de
	ld	(hl),#1
tui_text_restore:
	pop	ix
	pop	de			; the text pointer
tui_text_return_ok:
	ld	de,#0
	jr	tui_text_return
tui_text_einval:
	ld	de,#0xfffe
tui_text_return:
	pop	hl
	pop	af
	pop	af
	jp	(hl)
