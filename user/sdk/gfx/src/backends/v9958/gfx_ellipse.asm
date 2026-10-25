; -----------------------------------------------------------------------------
; libgfx: the ellipses (docs/gfx-sdk.md section 8.1).
;
; int gfx_ellipse(uint16_t cx, uint16_t cy, uint16_t rx, uint16_t ry,
;                 gfx_color_t color, enum gfx_op op);
; int gfx_fill_ellipse(... the same ...);
;
; The midpoint walk over one quadrant, incremental: region 1 steps X from
; the top to the 45-degree point, region 2 steps Y down to the axis; the
; second differences (2*ry^2*x and 2*rx^2*y) are carried, so the loops add
; and never multiply.  The decision terms are 32-bit, modulo 2^32: every
; term the walk compares stays inside a signed 32-bit integer for the
; radii the 512 x 512 space admits, so the wrapped products of the setup
; give the exact values.
;
; The walk records each row's half-width w[k] (k = 0..ry, the largest x
; visited on that row; x only grows as y falls), then the rows are drawn
; top to bottom, each mirrored across the centre:
;   - the fill: one span [cx - w, cx + w];
;   - the outline: the fill's boundary, the dots with a 4-neighbour
;     outside the fill: the outermost rows whole, the others a left and a
;     right piece max(1, w[k] - w[k+1]) dots long (merged when w is 0).
; At most two spans per row, each dot drawn once, so an XOR ellipse drawn
; twice restores the screen.  A COPY span is an HMMV over its byte-aligned
; dots (an LMMV for an odd GRAPHIC 6 end dot, and for a span shorter than
; four dots there); XOR is an LMMV.
; -----------------------------------------------------------------------------

	.module gfx_ellipse

	.globl _gfx_ellipse
	.globl _gfx_fill_ellipse

	.globl gfx_cmd_block
	.globl gfx_cmd_commit_short
	.globl gfx_take_whco
	.globl gfx_shape_op
	.globl gfx_op_nibble
	.globl gfx_line_body
	.globl gfx_point_body
	.globl gfx_ret_zero
	.globl gfx_ret_einval
	.globl gfx_a_x
	.globl gfx_a_y
	.globl gfx_a_w
	.globl gfx_a_h
	.globl gfx_a_c
	.globl gfx_a_op
	.globl _gfx_draw_mode
	.globl _gfx_draw_width
	.globl _gfx_batch_depth
	.globl _gfx_v9958_port_indirect
	.globl gfx_wait_ce

GFX_CMD_HMMV            = 0xC0
GFX_CMD_LMMV            = 0x80

	.area _DATA

ell_cx:
	.ds 2
ell_cy:
	.ds 2
ell_rx:
	.ds 2
ell_ry:
	.ds 2
ell_filled:
	.ds 1
ell_hmmv_clr:
	.ds 1			; the COPY HMMV colour byte

; 32-bit, little-endian
ell_rx2:
	.ds 4
ell_ry2:
	.ds 4
ell_trx2:
	.ds 4			; 2 * rx^2
ell_try2:
	.ds 4			; 2 * ry^2
ell_d:
	.ds 4			; the decision term
ell_dx2:
	.ds 4			; 2 * ry^2 * x
ell_dy2:
	.ds 4			; 2 * rx^2 * y
mul_a:
	.ds 4
mul_b:
	.ds 4
mul_r:
	.ds 4

ell_x:
	.ds 2
ell_y:
	.ds 2
ell_k:
	.ds 2
ell_line:
	.ds 2
ell_span_a:
	.ds 2
ell_span_b:
	.ds 2
ell_row_a:
	.ds 2			; the row's cx - w and cx + w
ell_row_b:
	.ds 2
ell_cmd:
	.ds 1			; the span's command code
ell_clr:
	.ds 1			; and its colour byte

; The half-widths, one word per row k = 0..ry (ry <= 511).
ell_w:
	.ds 1024

	.area _CODE

; ---------------------------------------------------------------------------
; 32-bit helpers over memory: (DE) op= (HL).  Clobber A, DE, HL.
; ---------------------------------------------------------------------------
ell_add32:
	ld	a,(de)
	add	a,(hl)
	ld	(de),a
	inc	de
	inc	hl
	ld	a,(de)
	adc	a,(hl)
	ld	(de),a
	inc	de
	inc	hl
	ld	a,(de)
	adc	a,(hl)
	ld	(de),a
	inc	de
	inc	hl
	ld	a,(de)
	adc	a,(hl)
	ld	(de),a
	ret

ell_sub32:
	ld	a,(de)
	sub	(hl)
	ld	(de),a
	inc	de
	inc	hl
	ld	a,(de)
	sbc	a,(hl)
	ld	(de),a
	inc	de
	inc	hl
	ld	a,(de)
	sbc	a,(hl)
	ld	(de),a
	inc	de
	inc	hl
	ld	a,(de)
	sbc	a,(hl)
	ld	(de),a
	ret

; (DE) = (HL), four bytes.  Clobbers BC.
ell_copy32:
	ldi
	ldi
	ldi
	ldi
	ret

; (HL) = the 16-bit DE, zero-extended.
ell_set32:
	ld	(hl),e
	inc	hl
	ld	(hl),d
	inc	hl
	ld	(hl),#0
	inc	hl
	ld	(hl),#0
	ret

; mul_r = mul_a * mul_b modulo 2^32 (shift and add over mul_b's set bits;
; mul_a and mul_b are consumed).
ell_mul32:
	ld	hl,#0
	ld	(mul_r),hl
	ld	(mul_r+2),hl
ell_mul_loop:
	ld	hl,(mul_b)
	ld	a,h
	or	l
	ld	hl,(mul_b+2)
	or	h
	or	l
	ret	z
	ld	hl,#mul_b+3
	srl	(hl)
	dec	hl
	rr	(hl)
	dec	hl
	rr	(hl)
	dec	hl
	rr	(hl)
	jr	nc,ell_mul_shift
	ld	de,#mul_r
	ld	hl,#mul_a
	call	ell_add32
ell_mul_shift:
	ld	hl,#mul_a
	sla	(hl)
	inc	hl
	rl	(hl)
	inc	hl
	rl	(hl)
	inc	hl
	rl	(hl)
	jr	ell_mul_loop

; mul_r = DE * DE (16 x 16), the square of a coordinate.
ell_square16:
	push	de
	ld	hl,#mul_a
	call	ell_set32
	pop	de
	ld	hl,#mul_b
	call	ell_set32
	jr	ell_mul32

; w[(ell_y)] = (ell_x)
ell_record:
	ld	hl,(ell_y)
	add	hl,hl
	ld	de,#ell_w
	add	hl,de
	ld	de,(ell_x)
	ld	(hl),e
	inc	hl
	ld	(hl),d
	ret

; ---------------------------------------------------------------------------
; The entry points.
; ---------------------------------------------------------------------------
_gfx_ellipse::
	xor	a
	jr	ell_entry
_gfx_fill_ellipse::
	ld	a,#1
ell_entry:
	call	gfx_take_whco		; cx, cy, rx (w), ry (h)
	ld	(ell_filled),a
	call	gfx_shape_op
	ret	c
	; the bounding box: cx >= rx, cx + rx < width, cy >= ry, cy + ry <= 511
	ld	hl,(gfx_a_x)
	ld	de,(gfx_a_w)
	or	a
	sbc	hl,de
	jp	c,gfx_ret_einval
	ld	hl,(gfx_a_x)
	add	hl,de
	ld	de,(_gfx_draw_width)
	or	a
	sbc	hl,de
	jp	nc,gfx_ret_einval
	ld	hl,(gfx_a_y)
	ld	de,(gfx_a_h)
	or	a
	sbc	hl,de
	jp	c,gfx_ret_einval
	ld	hl,(gfx_a_y)
	add	hl,de
	ld	de,#512
	or	a
	sbc	hl,de
	jp	nc,gfx_ret_einval
	; keep the geometry
	ld	hl,(gfx_a_x)
	ld	(ell_cx),hl
	ld	hl,(gfx_a_y)
	ld	(ell_cy),hl
	ld	hl,(gfx_a_w)
	ld	(ell_rx),hl
	ld	hl,(gfx_a_h)
	ld	(ell_ry),hl
	; the degenerate point and lines
	ld	hl,(ell_rx)
	ld	a,h
	or	l
	jr	nz,ell_rx_set
	ld	hl,(ell_ry)
	ld	a,h
	or	l
	jp	z,gfx_point_body	; (cx, cy): gfx_a_x/y already hold it
	; the vertical line (cx, cy - ry) - (cx, cy + ry)
	ld	hl,(ell_cx)
	ld	(gfx_a_w),hl
	ld	hl,(ell_cy)
	ld	de,(ell_ry)
	or	a
	sbc	hl,de
	ld	(gfx_a_y),hl
	ld	hl,(ell_cy)
	add	hl,de
	ld	(gfx_a_h),hl
	jp	gfx_line_body
ell_rx_set:
	ld	hl,(ell_ry)
	ld	a,h
	or	l
	jr	nz,ell_walk
	; the horizontal line (cx - rx, cy) - (cx + rx, cy)
	ld	hl,(ell_cx)
	ld	de,(ell_rx)
	or	a
	sbc	hl,de
	ld	(gfx_a_x),hl
	ld	hl,(ell_cx)
	add	hl,de
	ld	(gfx_a_w),hl
	ld	hl,(ell_cy)
	ld	(gfx_a_h),hl
	jp	gfx_line_body

; ---------------------------------------------------------------------------
; The walk.
; ---------------------------------------------------------------------------
ell_walk:
	; rx2, ry2, 2rx2, 2ry2
	ld	de,(ell_rx)
	call	ell_square16
	ld	de,#ell_rx2
	ld	hl,#mul_r
	call	ell_copy32
	ld	de,(ell_ry)
	call	ell_square16
	ld	de,#ell_ry2
	ld	hl,#mul_r
	call	ell_copy32
	ld	de,#ell_trx2
	ld	hl,#ell_rx2
	call	ell_copy32
	ld	de,#ell_trx2
	ld	hl,#ell_rx2
	call	ell_add32
	ld	de,#ell_try2
	ld	hl,#ell_ry2
	call	ell_copy32
	ld	de,#ell_try2
	ld	hl,#ell_ry2
	call	ell_add32
	; P = rx2 * ry: dy2 = 2P, d = ry2 - P + rx2 / 4
	ld	de,#mul_a
	ld	hl,#ell_rx2
	call	ell_copy32
	ld	de,(ell_ry)
	ld	hl,#mul_b
	call	ell_set32
	call	ell_mul32
	ld	de,#ell_dy2
	ld	hl,#mul_r
	call	ell_copy32
	ld	de,#ell_dy2
	ld	hl,#mul_r
	call	ell_add32
	ld	de,#ell_d
	ld	hl,#ell_ry2
	call	ell_copy32
	ld	de,#ell_d
	ld	hl,#mul_r
	call	ell_sub32
	ld	de,#mul_a		; rx2 >> 2
	ld	hl,#ell_rx2
	call	ell_copy32
	ld	b,#2
ell_quarter:
	ld	hl,#mul_a+3
	srl	(hl)
	dec	hl
	rr	(hl)
	dec	hl
	rr	(hl)
	dec	hl
	rr	(hl)
	djnz	ell_quarter
	ld	de,#ell_d
	ld	hl,#mul_a
	call	ell_add32
	; x = 0, y = ry, dx2 = 0
	ld	hl,#0
	ld	(ell_x),hl
	ld	(ell_dx2),hl
	ld	(ell_dx2+2),hl
	ld	hl,(ell_ry)
	ld	(ell_y),hl

	; region 1: while dx2 <= dy2
ell_region1:
	ld	hl,#ell_dy2		; dy2 - dx2: a borrow ends the region
	ld	de,#ell_dx2
	ld	a,(de)
	ld	c,a
	ld	a,(hl)
	sub	c
	inc	hl
	inc	de
	ld	a,(de)
	ld	c,a
	ld	a,(hl)
	sbc	a,c
	inc	hl
	inc	de
	ld	a,(de)
	ld	c,a
	ld	a,(hl)
	sbc	a,c
	inc	hl
	inc	de
	ld	a,(de)
	ld	c,a
	ld	a,(hl)
	sbc	a,c
	jr	c,ell_region2
	call	ell_record
	ld	hl,(ell_x)
	inc	hl
	ld	(ell_x),hl
	ld	de,#ell_dx2
	ld	hl,#ell_try2
	call	ell_add32
	ld	a,(ell_d+3)
	rla
	jr	nc,ell_r1_step_y
	; d < 0: d += dx2 + ry2
	ld	de,#ell_d
	ld	hl,#ell_dx2
	call	ell_add32
	ld	de,#ell_d
	ld	hl,#ell_ry2
	call	ell_add32
	jr	ell_region1
ell_r1_step_y:
	; y--, dy2 -= 2rx2, d += dx2 + ry2 - dy2
	ld	hl,(ell_y)
	dec	hl
	ld	(ell_y),hl
	ld	de,#ell_dy2
	ld	hl,#ell_trx2
	call	ell_sub32
	ld	de,#ell_d
	ld	hl,#ell_dx2
	call	ell_add32
	ld	de,#ell_d
	ld	hl,#ell_ry2
	call	ell_add32
	ld	de,#ell_d
	ld	hl,#ell_dy2
	call	ell_sub32
	jr	ell_region1

	; region 2: d = ry2 * (x - 1)^2 + rx2 * y^2 - rx2 * ry2
ell_region2:
	ld	de,(ell_x)
	dec	de
	call	ell_square16
	ld	de,#mul_b
	ld	hl,#mul_r
	call	ell_copy32
	ld	de,#mul_a
	ld	hl,#ell_ry2
	call	ell_copy32
	call	ell_mul32
	ld	de,#ell_d
	ld	hl,#mul_r
	call	ell_copy32
	ld	de,(ell_y)
	call	ell_square16
	ld	de,#mul_b
	ld	hl,#mul_r
	call	ell_copy32
	ld	de,#mul_a
	ld	hl,#ell_rx2
	call	ell_copy32
	call	ell_mul32
	ld	de,#ell_d
	ld	hl,#mul_r
	call	ell_add32
	ld	de,#mul_a
	ld	hl,#ell_rx2
	call	ell_copy32
	ld	de,#mul_b
	ld	hl,#ell_ry2
	call	ell_copy32
	call	ell_mul32
	ld	de,#ell_d
	ld	hl,#mul_r
	call	ell_sub32
ell_region2_loop:
	ld	hl,(ell_y)
	ld	a,h
	or	l
	jr	z,ell_walk_end
	call	ell_record
	ld	hl,(ell_y)
	dec	hl
	ld	(ell_y),hl
	ld	de,#ell_dy2		; 2rx2 * y, y the new row
	ld	hl,#ell_trx2
	call	ell_sub32
	; d > 0 (signed): d += rx2 - dy2; else also + dx2 (the old x), x++
	ld	a,(ell_d+3)
	bit	7,a
	jr	nz,ell_r2_step_x
	ld	hl,(ell_d)
	or	h
	or	l
	ld	hl,(ell_d+2)
	or	h
	or	l
	jr	z,ell_r2_step_x
	ld	de,#ell_d
	ld	hl,#ell_rx2
	call	ell_add32
	ld	de,#ell_d
	ld	hl,#ell_dy2
	call	ell_sub32
	jr	ell_region2_loop
ell_r2_step_x:
	ld	de,#ell_d
	ld	hl,#ell_rx2
	call	ell_add32
	ld	de,#ell_d
	ld	hl,#ell_dy2
	call	ell_sub32
	ld	de,#ell_d
	ld	hl,#ell_dx2
	call	ell_add32
	ld	hl,(ell_x)
	inc	hl
	ld	(ell_x),hl
	ld	de,#ell_dx2
	ld	hl,#ell_try2
	call	ell_add32
	jr	ell_region2_loop
ell_walk_end:
	call	ell_record		; the axis row, y = 0

; ---------------------------------------------------------------------------
; The rows, top to bottom: k = ry..1 above the centre, 0, then 1..ry.
; ---------------------------------------------------------------------------
	; the COPY HMMV colour: G6 packs the pixel in both nibbles
	ld	a,(gfx_a_c)
	ld	c,a
	ld	a,(_gfx_draw_mode)
	or	a
	ld	a,c
	jr	nz,ell_clr_set
	add	a,a
	add	a,a
	add	a,a
	add	a,a
	or	c
ell_clr_set:
	ld	(ell_hmmv_clr),a
	; the spans' constant fields: one line each, no argument bits
	ld	hl,#1
	ld	(gfx_cmd_block+10),hl	; NY
	xor	a
	ld	(gfx_cmd_block+13),a	; ARG
	ld	hl,(ell_ry)
	ld	(ell_k),hl
ell_top:
	ld	hl,(ell_cy)
	ld	de,(ell_k)
	or	a
	sbc	hl,de
	ld	(ell_line),hl
	call	ell_row
	ld	hl,(ell_k)
	dec	hl
	ld	(ell_k),hl
	ld	a,h
	or	l
	jr	nz,ell_top
	ld	hl,(ell_cy)		; k = 0
	ld	(ell_line),hl
	call	ell_row
ell_bottom:
	ld	hl,(ell_k)
	inc	hl
	ld	(ell_k),hl
	ld	hl,(ell_cy)
	ld	de,(ell_k)
	add	hl,de
	ld	(ell_line),hl
	call	ell_row
	ld	hl,(ell_k)
	ld	de,(ell_ry)
	or	a
	sbc	hl,de
	jr	nz,ell_bottom
	jp	gfx_ret_zero

; One row: k in ell_k, the scanline in ell_line.
ell_row:
	ld	hl,(ell_k)
	add	hl,hl
	ld	de,#ell_w
	add	hl,de
	ld	e,(hl)
	inc	hl
	ld	d,(hl)			; DE = w[k]
	inc	hl
	ld	c,(hl)
	inc	hl
	ld	b,(hl)			; BC = w[k + 1] (when k < ry)
	ld	hl,(ell_cx)
	or	a
	sbc	hl,de
	ld	(ell_row_a),hl		; cx - w
	ld	hl,(ell_cx)
	add	hl,de
	ld	(ell_row_b),hl		; cx + w
	ld	a,(ell_filled)
	or	a
	jr	nz,ell_row_whole
	ld	a,d
	or	e
	jr	z,ell_row_whole		; w = 0: the single dot
	push	de
	ld	hl,(ell_k)
	ld	de,(ell_ry)
	or	a
	sbc	hl,de
	pop	de
	jr	z,ell_row_whole		; the outermost rows
	; L = max(1, w - w[k + 1])
	ex	de,hl
	or	a
	sbc	hl,bc
	ld	a,h
	or	l
	jr	nz,ell_row_pieces
	inc	hl
ell_row_pieces:
	dec	hl			; L - 1
	push	hl
	ld	de,(ell_row_a)		; the left piece [a, a + L - 1]
	add	hl,de
	ex	de,hl
	ld	hl,(ell_row_a)
	call	ell_span
	pop	de			; the right piece [b - (L - 1), b]
	ld	hl,(ell_row_b)
	or	a
	sbc	hl,de
	ld	de,(ell_row_b)
	jr	ell_span
ell_row_whole:
	ld	hl,(ell_row_a)
	ld	de,(ell_row_b)
	; fall through

; ell_span: HL = the first dot, DE = the last, on ell_line.
ell_span:
	ld	(ell_span_a),hl
	ld	(ell_span_b),de
	ld	a,(gfx_a_op)
	or	a
	jr	nz,ell_span_lmmv	; XOR: per dot
	ld	a,(_gfx_draw_mode)
	or	a
	jr	nz,ell_span_hmmv	; G7: one dot per byte
	; G6: HMMV over the whole bytes, LMMV for an odd end dot; a span
	; shorter than four dots is one LMMV
	ex	de,hl
	or	a
	sbc	hl,de			; last - first
	ld	a,h
	or	a
	jr	nz,ell_span_g6
	ld	a,l
	cp	#3
	jr	c,ell_span_lmmv
ell_span_g6:
	ld	a,(ell_span_a)
	rrca
	jr	nc,ell_span_g6_right
	ld	hl,(ell_span_a)		; the odd first dot
	ld	d,h
	ld	e,l
	call	ell_issue_lmmv
	ld	hl,(ell_span_a)
	inc	hl
	ld	(ell_span_a),hl
ell_span_g6_right:
	ld	a,(ell_span_b)
	rrca
	jr	c,ell_span_g6_mid
	ld	hl,(ell_span_b)		; the even last dot
	ld	d,h
	ld	e,l
	call	ell_issue_lmmv
	ld	hl,(ell_span_b)
	dec	hl
	ld	(ell_span_b),hl
ell_span_g6_mid:
	ld	hl,(ell_span_a)
	ld	de,(ell_span_b)
ell_span_hmmv:
	ld	a,(ell_hmmv_clr)
	ld	(ell_clr),a
	ld	a,#GFX_CMD_HMMV
	jr	ell_issue
ell_span_lmmv:
	ld	hl,(ell_span_a)
	ld	de,(ell_span_b)
ell_issue_lmmv:
	ld	a,(gfx_a_c)
	ld	(ell_clr),a
	call	gfx_op_nibble
	or	#GFX_CMD_LMMV
; ell_issue: A = the command code, HL..DE the dots on ell_line.  Outside
; a batch the span goes to the port from the registers (R#36..R#46,
; NY 1 and ARG 0); inside one it is built and queued.
ell_issue:
	ld	(ell_cmd),a
	ex	de,hl
	or	a
	sbc	hl,de
	inc	hl			; HL = NX, DE = DX
	ld	a,(_gfx_batch_depth)
	or	a
	jr	nz,ell_issue_queue
	call	gfx_wait_ce		; C = the command port
	ld	a,#36
	out	(c),a
	ld	a,#0x91
	out	(c),a
	ld	a,(_gfx_v9958_port_indirect)
	ld	c,a
	ld	b,#0
	di
	out	(c),e			; DX
	out	(c),d
	ld	a,(ell_line)		; DY
	out	(c),a
	ld	a,(ell_line+1)
	out	(c),a
	out	(c),l			; NX
	out	(c),h
	ld	a,#1			; NY
	out	(c),a
	out	(c),b
	ld	a,(ell_clr)		; CLR
	out	(c),a
	out	(c),b			; ARG
	ld	a,(ell_cmd)		; CMD: the command starts
	out	(c),a
	ei
	ret
ell_issue_queue:
	ld	(gfx_cmd_block+8),hl	; NX
	ex	de,hl
	ld	(gfx_cmd_block+4),hl	; DX
	ld	hl,(ell_line)
	ld	(gfx_cmd_block+6),hl	; DY
	ld	a,(ell_clr)
	ld	(gfx_cmd_block+12),a
	ld	a,(ell_cmd)
	ld	(gfx_cmd_block+14),a
	jp	gfx_cmd_commit_short
