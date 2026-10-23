; -----------------------------------------------------------------------------
; Semantic one-row scrolling for render-capable consoles.
;
; int tui_scroll_rows(uint8_t top, uint8_t height, uint8_t direction)
;   sdcccall(1): A = top, L = height, direction is one byte on the stack.
;
; This is an opt-in archive member.  It flushes pending cells, submits one
; ten-byte SCROLL render transaction, then shifts the clean logical cell map.
; The small persistent stream doubles as scratch after the transaction; libtui
; is deliberately non-reentrant, like the rest of its static screen model.
; -----------------------------------------------------------------------------

	.module tui_scroll
	.globl _tui_scroll_rows
	.globl _tui_flush
	.globl _console_render
	.globl _tui_insert_line
	.globl _tui_delete_line
	.globl _tui_state

; Stable private tui_state offsets.  model_only is last so existing offsets
; stay unchanged.
TUI_INITIALIZED       = 0
TUI_SCREEN_COLS       = 1
TUI_SCREEN_ROWS       = 2
TUI_WINDOW_X          = 3
TUI_WINDOW_Y          = 4
TUI_WINDOW_W          = 5
TUI_WINDOW_H          = 6
TUI_CURSOR_X          = 7
TUI_CURSOR_Y          = 8
TUI_ATTR              = 11
TUI_RENDER_SUPPORT    = 36
TUI_MODEL_ONLY        = 37

KCONSOLE_RENDER_VERSION     = 1
KCONSOLE_RENDER_SCROLL      = 4
KCONSOLE_RENDER_SCROLL_SIZE = 6
TUI_SCROLL_DOWN              = 1

	.area _DATA
tui_scroll_stream:
	.ds 10

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
	cp	#TUI_SCROLL_DOWN+1
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

	; Preserve the caller's window and logical cursor in bytes 0..5, install
	; the absolute full-width scroll region, and reuse the established line
	; operation under model_only.  The successful hardware transaction has
	; already made the corresponding pixels visible, so no dirty cells are
	; scheduled.
	ld	hl,#_tui_state+TUI_WINDOW_X
	ld	de,#tui_scroll_stream
	ld	bc,#6
	ldir
	xor	a
	ld	(_tui_state+TUI_WINDOW_X),a
	ld	a,(tui_scroll_stream+6)
	ld	(_tui_state+TUI_WINDOW_Y),a
	ld	a,(_tui_state+TUI_SCREEN_COLS)
	ld	(_tui_state+TUI_WINDOW_W),a
	ld	a,(tui_scroll_stream+7)
	ld	(_tui_state+TUI_WINDOW_H),a
	xor	a
	ld	(_tui_state+TUI_CURSOR_X),a
	ld	(_tui_state+TUI_CURSOR_Y),a
	inc	a
	ld	(_tui_state+TUI_MODEL_ONLY),a
	ld	a,(tui_scroll_stream+8)
	or	a
	jr	nz,tui_scroll_model_down
	call	_tui_delete_line
	jr	tui_scroll_model_done
tui_scroll_model_down:
	call	_tui_insert_line
tui_scroll_model_done:
	xor	a
	ld	(_tui_state+TUI_MODEL_ONLY),a
	ld	hl,#tui_scroll_stream
	ld	de,#_tui_state+TUI_WINDOW_X
	ld	bc,#6
	ldir

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
