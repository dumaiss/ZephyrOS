; -----------------------------------------------------------------------------
; Semantic one-row scrolling for render-capable consoles.
;
; int tui_scroll_rows(uint8_t top, uint8_t height, uint8_t direction)
;   sdcccall(1): A = top, L = height, direction is one byte on the stack.
;
; This is an opt-in archive member.  It flushes pending cells, submits one
; ten-byte SCROLL render transaction, then shifts the clean logical cell map:
; the characters move as one (height - 1) x stride block (LDIR, or LDDR for
; down) and the 4-bit-per-cell attributes move as half that block (the even
; stride keeps every row on an attribute-byte boundary).  The exposed row
; holds spaces in the current attribute across the screen width; the dirty
; and invert maps stay clear.  The small persistent stream doubles as
; scratch after the transaction; libtui is deliberately non-reentrant, like
; the rest of its static screen model.
; -----------------------------------------------------------------------------

	.module tui_scroll
	.globl _tui_scroll_rows
	.globl _tui_flush
	.globl _console_render
	.globl __mulint
	.globl _tui_state
	.globl _tui_chars
	.globl _tui_attrs

; Stable private tui_state offsets.  stride is last so existing offsets
; stay unchanged.
TUI_INITIALIZED       = 0
TUI_SCREEN_COLS       = 1
TUI_SCREEN_ROWS       = 2
TUI_ATTR              = 11
TUI_RENDER_SUPPORT    = 36
TUI_STRIDE            = 38

KCONSOLE_RENDER_VERSION     = 1
KCONSOLE_RENDER_SCROLL      = 4
KCONSOLE_RENDER_SCROLL_SIZE = 6

	.area _DATA
tui_scroll_stream:
	.ds 10
tui_scroll_top:
	.ds 2   ; top x stride, the region's first cell
tui_scroll_n:
	.ds 2   ; (height - 1) x stride cells
tui_scroll_parity:
	.ds 1   ; the exposed row's cell nibble parity
tui_scroll_count:
	.ds 2   ; the exposed row's remaining cells
tui_scroll_attr:
	.ds 1   ; the exposed row's attribute

	.area _CODE

_tui_scroll_rows::
	; Save the register arguments and fetch the one-byte stack argument.
	ld	(tui_scroll_stream+6),a
	ld	a,l
	ld	(tui_scroll_stream+7),a
	ld	h,#0
	ld	l,#2
	add	hl,sp
	ld	a,(hl)
	ld	(tui_scroll_stream+8),a

	; Validate before a flush can cause any observable work.
	ld	a,(_tui_state+TUI_INITIALIZED)
	or	a
	jp	z,tui_scroll_einval
	ld	a,(tui_scroll_stream+7)
	or	a
	jp	z,tui_scroll_einval
	ld	a,(tui_scroll_stream+6)
	ld	b,a
	ld	a,(_tui_state+TUI_SCREEN_ROWS)
	cp	b
	jp	c,tui_scroll_einval
	jp	z,tui_scroll_einval
	ld	a,(tui_scroll_stream+7)
	add	a,b
	ld	b,a
	ld	a,(_tui_state+TUI_SCREEN_ROWS)
	cp	b
	jp	c,tui_scroll_einval
	ld	a,(tui_scroll_stream+8)
	cp	#2
	jp	nc,tui_scroll_einval

	call	_tui_flush
	bit	7,d
	jp	nz,tui_scroll_return
	ld	a,(_tui_state+TUI_RENDER_SUPPORT)
	dec	a
	jp	nz,tui_scroll_enotsup

	; Header followed by one six-byte SCROLL record.  Bytes 6..9 already
	; contain top, height, direction; finish with the current attribute.
	ld	a,#KCONSOLE_RENDER_VERSION
	ld	(tui_scroll_stream+0),a
	xor	a
	ld	(tui_scroll_stream+1),a
	ld	a,#10
	ld	(tui_scroll_stream+2),a
	xor	a
	ld	(tui_scroll_stream+3),a
	ld	a,#KCONSOLE_RENDER_SCROLL
	ld	(tui_scroll_stream+4),a
	ld	a,#KCONSOLE_RENDER_SCROLL_SIZE
	ld	(tui_scroll_stream+5),a
	ld	a,(_tui_state+TUI_ATTR)
	ld	(tui_scroll_stream+9),a
	ld	hl,#tui_scroll_stream
	ld	de,#10
	call	_console_render
	bit	7,d
	jp	nz,tui_scroll_return
	ld	a,d
	or	a
	jp	nz,tui_scroll_eio
	ld	a,e
	cp	#10
	jp	nz,tui_scroll_eio

	; The hardware transaction has already made the pixels visible, so
	; the clean model moves without scheduling any dirty cell.  Compute
	; the region geometry once: stride, top x stride and the block size.
	ld	a,(_tui_state+TUI_STRIDE)
	ld	e,a
	ld	d,#0
	ld	a,(tui_scroll_stream+6)
	ld	l,a
	ld	h,#0
	call	__mulint		; DE = top x stride
	ld	(tui_scroll_top),de
	ld	a,(tui_scroll_stream+7)
	dec	a
	ld	l,a
	ld	h,#0
	ld	a,(_tui_state+TUI_STRIDE)
	ld	e,a
	ld	d,#0
	call	__mulint		; DE = (height - 1) x stride
	ld	(tui_scroll_n),de

	; The character block: one LDIR for up, one LDDR for down.
	ld	bc,(tui_scroll_n)
	ld	hl,(tui_scroll_top)
	ld	de,#_tui_chars
	add	hl,de
	ld	a,(tui_scroll_stream+8)
	or	a
	jr	nz,tui_scroll_chars_down
	; Up: destination is the region's top row, source one stride below.
	push	hl			; destination base
	ld	a,(_tui_state+TUI_STRIDE)
	add	a,l
	ld	l,a
	ld	a,h
	adc	a,#0
	ld	h,a			; HL = source base
	pop	de			; DE = destination base
	ldir
	jr	tui_scroll_attrs

tui_scroll_chars_down:
	; Down: source is the region's top row, destination one stride below.
	push	hl			; source base
	add	hl,bc
	dec	hl			; HL = source end
	pop	de			; DE = source base
	ld	a,(_tui_state+TUI_STRIDE)
	add	a,e
	ld	e,a
	ld	a,d
	adc	a,#0
	ld	d,a			; DE = destination base
	ex	de,hl			; DE = source end, HL = destination base
	add	hl,bc
	dec	hl			; HL = destination end
	ex	de,hl			; DE = destination end, HL = source end
	lddr

tui_scroll_attrs:
	; The stride is even, so every row starts on an attribute-byte
	; boundary: the attributes move as one (height - 1) x stride / 2
	; byte block, LDIR for up and LDDR for down.
	ld	hl,(tui_scroll_n)
	srl	h
	rr	l
	ld	b,h
	ld	c,l			; BC = N / 2
	ld	a,(tui_scroll_stream+8)
	or	a
	jr	nz,tui_scroll_attrs_down
	ld	hl,(tui_scroll_top)
	srl	h
	rr	l			; HL = top x stride / 2
	ld	de,#_tui_attrs
	add	hl,de			; HL = destination byte
	push	hl
	ld	a,(_tui_state+TUI_STRIDE)
	srl	a
	add	a,l
	ld	l,a
	ld	a,h
	adc	a,#0
	ld	h,a			; HL = source byte
	pop	de			; DE = destination byte
	ldir
	jr	tui_scroll_attrs_done

tui_scroll_attrs_down:
	ld	hl,(tui_scroll_top)
	srl	h
	rr	l			; HL = top x stride / 2
	ld	de,#_tui_attrs
	add	hl,de			; HL = source byte
	push	hl
	ld	a,(_tui_state+TUI_STRIDE)
	srl	a
	add	a,l
	ld	l,a
	ld	a,h
	adc	a,#0
	ld	h,a			; HL = destination byte
	pop	de			; DE = source byte
	ex	de,hl			; DE = destination, HL = source
	add	hl,bc
	dec	hl			; HL = source end
	ex	de,hl			; DE = source end, HL = destination
	add	hl,bc
	dec	hl			; HL = destination end
	ex	de,hl			; DE = destination end, HL = source end
	lddr

tui_scroll_attrs_done:
	; Blank the exposed row: spaces in the current attribute across the
	; screen width.  The row is top + height - 1 for up, top for down.
	ld	a,(tui_scroll_stream+8)
	or	a
	jr	nz,tui_scroll_blank_down
	ld	a,(tui_scroll_stream+6)
	ld	b,a
	ld	a,(tui_scroll_stream+7)
	add	a,b
	dec	a
	jr	tui_scroll_blank
tui_scroll_blank_down:
	ld	a,(tui_scroll_stream+6)
tui_scroll_blank:
	ld	l,a
	ld	h,#0
	ld	a,(_tui_state+TUI_STRIDE)
	ld	e,a
	ld	d,#0
	call	__mulint		; DE = row x stride
	ld	a,e
	and	#0x01
	ld	(tui_scroll_parity),a
	ld	hl,#_tui_chars
	add	hl,de
	ex	de,hl			; DE = characters, HL = row x stride
	srl	h
	rr	l
	ld	bc,#_tui_attrs
	add	hl,bc			; HL = attribute byte
	ld	a,(_tui_state+TUI_SCREEN_COLS)
	ld	(tui_scroll_count),a
	ld	a,(_tui_state+TUI_ATTR)
	ld	(tui_scroll_attr),a

tui_scroll_blank_loop:
	ld	a,#0x20
	ld	(de),a
	inc	de
	ld	a,(tui_scroll_parity)
	or	a
	jr	nz,tui_scroll_blank_high
	ld	a,(tui_scroll_attr)
	and	#0x0f
	ld	b,a
	ld	a,(hl)
	and	#0xf0
	or	b
	ld	(hl),a
	jr	tui_scroll_blank_par
tui_scroll_blank_high:
	ld	a,(tui_scroll_attr)
	rlca
	rlca
	rlca
	rlca
	and	#0xf0
	ld	b,a
	ld	a,(hl)
	and	#0x0f
	or	b
	ld	(hl),a
	inc	hl
tui_scroll_blank_par:
	ld	a,(tui_scroll_parity)
	xor	#0x01
	ld	(tui_scroll_parity),a
	ld	a,(tui_scroll_count)
	dec	a
	ld	(tui_scroll_count),a
	jr	nz,tui_scroll_blank_loop

tui_scroll_ok:
	ld	de,#0
	jr	tui_scroll_return
tui_scroll_einval:
	ld	de,#0xfffe
	jr	tui_scroll_return
tui_scroll_enotsup:
	ld	de,#0xfff9
	jr	tui_scroll_return
tui_scroll_eio:
	ld	de,#0xfff6
tui_scroll_return:
	; sdcccall(1) callee-cleans the one stacked byte argument.
	pop	hl
	inc	sp
	jp	(hl)
