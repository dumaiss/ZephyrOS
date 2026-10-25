; -----------------------------------------------------------------------------
; libgfx: the streaming pixel upload (docs/gfx-sdk.md section 10).
;
; int gfx_upload_begin(uint16_t x, uint16_t y, uint16_t width,
;                      uint16_t height);
; int gfx_upload_write(const void *data, uint16_t length);
; int gfx_upload_end(void);
;
; gfx_upload_begin names a rectangle; gfx_upload_write streams payload
; bytes through the data port with the position kept across calls, and
; gfx_upload_end closes the transfer.  The stream follows the mode's
; packing, so the transfer takes exactly bytes(width, height); a chunk
; that would run past the end is refused whole.  One VRAM address is set
; per row (one for the whole transfer when the rectangle spans the full
; width, whose rows are contiguous), each after the command engine
; finishes, so the bytes land after every command.  A pending batch is
; flushed first; the upload does not nest.
; -----------------------------------------------------------------------------

	.module gfx_upload

	.globl _gfx_upload_begin
	.globl _gfx_upload_write
	.globl _gfx_upload_end

	.globl gfx_check_xw
	.globl gfx_check_yh
	.globl gfx_wait_ce
	.globl gfx_ret_zero
	.globl gfx_ret_einval
	.globl _gfx_flush
	.globl _gfx_draw_ready
	.globl _gfx_draw_mode
	.globl _gfx_draw_width
	.globl _gfx_v9958_io_stream_vram

GFX_MODE_G6             = 0
GFX_MODE_YJK            = 2

	.area _DATA

up_active:
	.ds 1
up_full:
	.ds 1			; the rectangle spans the full width
up_addressed:
	.ds 1			; the full-width address is set
up_col:
	.ds 1			; the byte column of the rows' start
up_line:
	.ds 2			; the current row (0..511)
up_row_bytes:
	.ds 2
up_row_left:
	.ds 2
up_remaining:
	.ds 2
up_x:
	.ds 2
up_y:
	.ds 2
up_w:
	.ds 2
up_h:
	.ds 2
up_src:
	.ds 2
up_len:
	.ds 2
up_done:
	.ds 2

	.area _CODE

; ---------------------------------------------------------------------------
_gfx_upload_begin::
	ld	(up_x),hl
	ld	(up_y),de
	pop	bc
	pop	hl
	ld	(up_w),hl
	pop	hl
	ld	(up_h),hl
	push	bc
	ld	a,(_gfx_draw_ready)
	or	a
	jp	z,gfx_ret_einval
	ld	a,(up_active)
	or	a
	jp	nz,gfx_ret_einval
	ld	hl,(up_x)		; a zero size is refused, as the bounds
	ld	de,(up_w)
	call	gfx_check_xw
	or	a
	jp	nz,gfx_ret_einval
	ld	hl,(up_y)
	ld	de,(up_h)
	call	gfx_check_yh
	or	a
	jp	nz,gfx_ret_einval
	ld	a,(up_x)
	ld	c,a
	ld	a,(up_w)
	or	c
	ld	c,a
	ld	a,(_gfx_draw_mode)
	or	a
	jr	nz,up_not_g6
	ld	a,c
	rrca
	jp	c,gfx_ret_einval
	jr	up_start
up_not_g6:
	cp	#GFX_MODE_YJK
	jr	nz,up_start
	ld	a,c
	and	#3
	jp	nz,gfx_ret_einval
up_start:
	call	_gfx_flush
	call	gfx_wait_ce
	ld	a,#1
	ld	(up_active),a
	xor	a
	ld	(up_addressed),a
	ld	(up_full),a
	ld	hl,#0
	ld	(up_row_left),hl
	; full width: x == 0 and width == the mode's width
	ld	hl,(up_x)
	ld	a,h
	or	l
	jr	nz,up_geometry
	ld	hl,(up_w)
	ld	de,(_gfx_draw_width)
	or	a
	sbc	hl,de
	jr	nz,up_geometry
	ld	a,#1
	ld	(up_full),a
up_geometry:
	ld	hl,(up_y)
	ld	(up_line),hl
	ld	hl,(up_x)
	ld	de,(up_w)
	ld	a,(_gfx_draw_mode)
	or	a
	jr	nz,up_bytes
	srl	h			; GRAPHIC 6: two pixels per byte
	rr	l
	srl	d
	rr	e
up_bytes:
	ld	a,l
	ld	(up_col),a
	ld	(up_row_bytes),de
	; remaining = row_bytes * height, modulo 2^16 (shift and add)
	ld	hl,#0
	ld	bc,(up_h)
up_mul:
	ld	a,b
	or	c
	jr	z,up_mul_done
	srl	b
	rr	c
	jr	nc,up_mul_shift
	add	hl,de
up_mul_shift:
	ex	de,hl
	add	hl,hl
	ex	de,hl
	jr	up_mul
up_mul_done:
	ld	(up_remaining),hl
	jp	gfx_ret_zero

; up_address: the VRAM address of the current row's start, after the
; command engine.
up_address:
	call	gfx_wait_ce		; C = the command port
	ld	hl,(up_line)
	ld	a,l
	rlca
	rlca
	and	#3
	bit	0,h
	jr	z,up_r14
	or	#4
up_r14:
	out	(c),a
	ld	a,#0x8E
	out	(c),a
	ld	a,(up_col)
	out	(c),a
	ld	a,l
	and	#0x3F
	or	#0x40
	out	(c),a
	ret

; ---------------------------------------------------------------------------
_gfx_upload_write::
	ld	(up_src),hl
	ld	(up_len),de
	ld	a,(up_active)
	or	a
	jp	z,gfx_ret_einval
	ld	a,h
	or	l
	jr	nz,up_write_some
	ld	a,d
	or	e
	jp	nz,gfx_ret_einval
up_write_some:
	; a chunk past the end is refused whole
	ld	hl,(up_remaining)
	or	a
	sbc	hl,de
	jp	c,gfx_ret_einval
	ld	hl,#0
	ld	(up_done),hl
	ld	a,(up_full)
	or	a
	jr	z,up_write_rows
	ld	a,(up_addressed)
	or	a
	jr	nz,up_write_full
	call	up_address
	ld	a,#1
	ld	(up_addressed),a
up_write_full:
	ld	hl,(up_src)
	ld	de,(up_len)
	call	_gfx_v9958_io_stream_vram
	ld	hl,(up_remaining)
	ld	de,(up_len)
	or	a
	sbc	hl,de
	ld	(up_remaining),hl
	ld	de,(up_len)
	ret
up_write_rows:
	ld	hl,(up_len)
	ld	a,h
	or	l
	jr	z,up_write_end
	ld	hl,(up_row_left)
	ld	a,h
	or	l
	jr	nz,up_write_chunk
	call	up_address
	ld	hl,(up_row_bytes)
	ld	(up_row_left),hl
up_write_chunk:
	; chunk = min(length, row_left)
	ld	hl,(up_row_left)
	ld	de,(up_len)
	or	a
	sbc	hl,de
	jr	nc,up_chunk_len
	ld	de,(up_row_left)
up_chunk_len:
	push	de
	ld	hl,(up_src)
	call	_gfx_v9958_io_stream_vram
	pop	de
	ld	hl,(up_row_left)
	or	a
	sbc	hl,de
	ld	(up_row_left),hl
	ld	hl,(up_remaining)
	or	a
	sbc	hl,de
	ld	(up_remaining),hl
	ld	hl,(up_src)
	add	hl,de
	ld	(up_src),hl
	ld	hl,(up_done)
	add	hl,de
	ld	(up_done),hl
	ld	hl,(up_len)
	or	a
	sbc	hl,de
	ld	(up_len),hl
	; a finished row moves to the next line
	ld	hl,(up_row_left)
	ld	a,h
	or	l
	jr	nz,up_write_rows
	ld	hl,(up_remaining)
	ld	a,h
	or	l
	jr	z,up_write_rows
	ld	hl,(up_line)
	inc	hl
	ld	(up_line),hl
	jr	up_write_rows
up_write_end:
	ld	de,(up_done)
	ret

; ---------------------------------------------------------------------------
_gfx_upload_end::
	ld	a,(up_active)
	or	a
	jp	z,gfx_ret_einval
	xor	a
	ld	(up_active),a
	ld	hl,(up_remaining)
	ld	a,h
	or	l
	jp	nz,gfx_ret_einval	; bytes missing: cancelled
	jp	gfx_ret_zero
