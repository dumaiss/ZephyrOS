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
; One loop serves every case.  The cell index, nibble parity, bit mask and
; bitmap byte index are computed once, then each step advances by one cell
; with no multiply or call: BC walks the text, DE the attribute bytes
; (every second cell), and the character, bitmap and flag pointers live in
; the small scratch block.  IX and IY stay untouched (the ABI preserves
; them for the C caller's frame).
; -----------------------------------------------------------------------------

	.module tui_text
	.globl _tui_put_text
	.globl __mulint
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
tui_text_attr:   .ds 1 ; the cell attribute
tui_text_parity: .ds 1 ; 0 = low nibble cell, 1 = high nibble cell
tui_text_mask:   .ds 1 ; the dirty/invert bit of the current cell
tui_text_remain: .ds 1 ; remaining in-window cells
tui_text_dopend: .ds 1 ; 1 = a change also sets the row's pending flag
tui_text_pdone:  .ds 1 ; 1 = the row's pending flag is already set
tui_text_didx:   .ds 1 ; the dirty/invert byte of the current cell
tui_text_byte:   .ds 1 ; the current text byte (BC is scratch in changes)
tui_text_nib:    .ds 1 ; the merged attribute nibble
tui_text_text:   .ds 2 ; the text pointer
tui_text_chars:  .ds 2 ; the current cell's character pointer
tui_text_pend:   .ds 2 ; &tui_row_pending[row]

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
	ld	(tui_text_text),de

	; length = (SP+4), attr = (SP+5).
	ld	hl,#4
	add	hl,sp
	ld	e,(hl)
	inc	hl
	ld	a,(hl)
	ld	(tui_text_attr),a
	ld	a,e
	or	a
	jp	z,tui_text_return_ok

	; Fully clipped rows draw nothing.
	ld	a,(tui_text_y)
	ld	e,a
	ld	a,(_tui_state+TUI_WINDOW_H)
	cp	e
	jp	c,tui_text_return_ok
	jp	z,tui_text_return_ok
	; The in-window cells: min(length, window width - x).  Cells at and
	; past the right edge are clipped, so the loop never sees them.
	ld	a,(tui_text_x)
	ld	e,a
	ld	a,(_tui_state+TUI_WINDOW_W)
	sub	e
	jp	c,tui_text_return_ok
	ld	e,a			; E = window width - x
	ld	hl,#4
	add	hl,sp
	ld	a,(hl)			; length again
	cp	e
	jr	c,tui_text_len_ok
	ld	a,e
tui_text_len_ok:
	ld	(tui_text_remain),a
	or	a
	jp	z,tui_text_return_ok

	; The absolute row and its pending-flag byte.  The pending flag is
	; per-call state, so it starts clear for every row.
	ld	a,(tui_text_y)
	ld	e,a
	ld	a,(_tui_state+TUI_WINDOW_Y)
	add	a,e
	ld	(tui_text_row),a
	ld	l,a
	ld	h,#0
	ld	de,#_tui_row_pending
	add	hl,de
	ld	(tui_text_pend),hl
	ld	a,#0
	ld	(tui_text_pdone),a
	; DE = row x stride, then + window x + x: the first cell's index.
	ld	a,(tui_text_row)
	ld	l,a
	ld	h,#0
	ld	a,(_tui_state+TUI_STRIDE)
	ld	e,a
	ld	d,#0
	call	__mulint		; DE = row x stride
	ld	a,(_tui_state+TUI_WINDOW_X)
	ld	b,a
	ld	a,(tui_text_x)
	add	a,b			; window x + x fits one byte
	add	a,e
	ld	e,a
	ld	a,d
	adc	a,#0
	ld	d,a			; DE = base index

	; The character pointer, nibble parity, bit mask and bitmap byte.
	; With the even stride the index parity is the absolute column
	; parity: the first cell's nibble position depends on x only.
	ld	hl,#_tui_chars
	add	hl,de
	ld	(tui_text_chars),hl
	ld	a,e
	and	#0x01
	ld	(tui_text_parity),a
	ld	a,e
	and	#0x07
	ld	b,a
	ld	a,#0x01
	jr	z,tui_text_mask_done
tui_text_mask_loop:
	add	a,a
	djnz	tui_text_mask_loop
tui_text_mask_done:
	ld	(tui_text_mask),a
	ld	h,d
	ld	l,e
	srl	h
	rr	l
	srl	h
	rr	l
	srl	h
	rr l
	ld	a,l
	ld	(tui_text_didx),a
	; DE = the first cell's attribute byte pointer.
	ld	h,d
	ld	l,e
	srl	h
	rr	l
	ld	de,#_tui_attrs
	add	hl,de
	ex	de,hl
	ld	bc,(tui_text_text)	; BC = text

	; One in-window cell per iteration.
tui_text_cell:
	ld	hl,(tui_text_chars)
	ld	a,(bc)
	cp	(hl)
	jp	nz,tui_text_modified
	; Compare the cell's attribute nibble.
	ld	a,(tui_text_parity)
	or	a
	ld	a,(de)
	jr	nz,tui_text_cmp_hi
	and	#0x0f
	jr	tui_text_cmp_go
tui_text_cmp_hi:
	rrca
	rrca
	rrca
	rrca
	and	#0x0f
tui_text_cmp_go:
	ld	(tui_text_nib),a
	ld	hl,#tui_text_nib
	ld	a,(tui_text_attr)
	cp	(hl)
	jp	z,tui_text_step

	; A changed cell.  Mirror tui_priv_set_cell: store under model_only
	; without bitmap or flag work; otherwise flag the row and record the
	; change as the C reference does.
tui_text_modified:
	ld	a,(bc)
	ld	(tui_text_byte),a	; BC is scratch from here on
	ld	a,#0
	ld	(tui_text_dopend),a
	ld	a,(_tui_state+TUI_MODEL_ONLY)
	or	a
	jr	nz,tui_text_store
	ld	a,#1
	ld	(tui_text_dopend),a
	; Already dirty: store only.
	ld	a,(tui_text_didx)
	ld	c,a
	ld	b,#0
	ld	hl,#_tui_dirty
	add	hl,bc
	ld	a,(tui_text_mask)
	ld	b,a
	ld	a,(hl)
	and	b
	jr	nz,tui_text_store
	; Character change: set dirty, clear invert.
	ld	a,(tui_text_byte)
	push	hl
	ld	hl,(tui_text_chars)
	cp	(hl)
	pop	hl
	jr	z,tui_text_invert
	ld	a,(hl)
	or	b
	ld	(hl),a
	ld	a,(tui_text_didx)
	ld	c,a
	ld	b,#0
	ld	hl,#_tui_invert
	add	hl,bc
	ld	a,(tui_text_mask)
	cpl
	ld	b,a
	ld	a,(hl)
	and	b
	ld	(hl),a
	jr	tui_text_store

	; Attribute-only change: the two-valued model flips the invert bit.
tui_text_invert:
	ld	a,(tui_text_didx)
	ld	c,a
	ld	b,#0
	ld	hl,#_tui_invert
	add	hl,bc
	ld	a,(tui_text_mask)
	ld	b,a
	ld	a,(hl)
	and	b
	jr	nz,tui_text_inv_clr
	ld	a,(hl)
	or	b
	ld	(hl),a
	jr	tui_text_store
tui_text_inv_clr:
	ld	a,b
	cpl
	ld	b,a
	ld	a,(hl)
	and	b
	ld	(hl),a

tui_text_store:
	ld	bc,(tui_text_text)
	ld	a,(tui_text_byte)
	ld	hl,(tui_text_chars)
	ld	(hl),a
	ld	a,(tui_text_parity)
	or	a
	jr	nz,tui_text_st_hi
	ld	a,(tui_text_attr)
	and	#0x0f
	ld	(tui_text_nib),a
	ld	hl,#tui_text_nib
	ld	a,(de)
	and	#0xf0
	or	(hl)
	ld	(de),a
	jr	tui_text_st_pend
tui_text_st_hi:
	ld	a,(tui_text_attr)
	rlca
	rlca
	rlca
	rlca
	and	#0xf0
	ld	(tui_text_nib),a
	ld	hl,#tui_text_nib
	ld	a,(de)
	and	#0x0f
	or	(hl)
	ld	(de),a
tui_text_st_pend:
	ld	a,(tui_text_dopend)
	or	a
	jr	z,tui_text_step
	ld	a,(tui_text_pdone)
	or	a
	jr	nz,tui_text_step
	ld	a,#1
	ld	(tui_text_pdone),a
	ld	hl,(tui_text_pend)
	ld	(hl),a

	; One cell right: the pointers step, the mask and parity roll.
tui_text_step:
	ld	hl,(tui_text_chars)
	inc	hl
	ld	(tui_text_chars),hl
	inc	bc
	ld	(tui_text_text),bc
	ld	a,(tui_text_parity)
	xor	#0x01
	ld	(tui_text_parity),a
	jr	nz,tui_text_step_mask
	inc	de
tui_text_step_mask:
	ld	a,(tui_text_mask)
	add	a,a
	ld	(tui_text_mask),a
	jr	nz,tui_text_step_cnt
	ld	a,#0x01
	ld	(tui_text_mask),a
	ld	a,(tui_text_didx)
	inc	a
	ld	(tui_text_didx),a
tui_text_step_cnt:
	ld	a,(tui_text_remain)
	dec	a
	ld	(tui_text_remain),a
	jp	nz,tui_text_cell
	jp	tui_text_return_ok

tui_text_einval:
	ld	de,#0xfffe
	jr	tui_text_return
tui_text_return_ok:
	ld	de,#0
tui_text_return:
	pop	hl
	pop	af
	pop	af
	jp	(hl)
