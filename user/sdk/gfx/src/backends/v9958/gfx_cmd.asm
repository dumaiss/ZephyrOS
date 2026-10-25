; -----------------------------------------------------------------------------
; libgfx: the command path (docs/gfx-sdk.md sections 8, 8.1 and 9).
;
; The rectangle, line and point entry points, the batch and the command
; issue, in assembly from the public entry point to the port.  Each call
; validates its arguments in the order the API documents (a refusal
; queues nothing), builds the V9958's R#32..R#46 block in gfx_cmd_block
; and commits it: outside gfx_begin/gfx_end the block is issued at once
; (wait for CE, select R#17, OUTIs, the last one starting the command);
; inside a batch it is copied into the queue, which holds ready blocks
; (16-byte slots, 32 of them) and flushes itself when full.  The call
; returns with the command still drawing.  A command that reads no
; source (HMMV, LMMV, LINE, PSET) is issued from R#36, eleven bytes; a
; copy (HMMM, LMMM) from R#32, fifteen; the block's byte 15 says which,
; for the queue.
;
; The drawing state the entry points test is owned here: the C side
; (gfx.c, gfx_mode.c) writes it at open, mode set and close.
;
; Call convention (sdcccall(1)): the first two 16-bit arguments in HL and
; DE, the rest on the stack, two 8-bit arguments packed in one word; the
; callee pops its stack arguments; a 16-bit result in DE.  IX is
; preserved.
; -----------------------------------------------------------------------------

	.module gfx_cmd

	.globl _gfx_fill_rect
	.globl _gfx_clear
	.globl _gfx_copy_rect
	.globl _gfx_rect
	.globl _gfx_line
	.globl _gfx_point
	.globl _gfx_begin
	.globl _gfx_end
	.globl _gfx_flush

	.globl _gfx_draw_open
	.globl _gfx_draw_ready
	.globl _gfx_draw_mode
	.globl _gfx_draw_width
	.globl _gfx_batch_depth
	.globl _gfx_batch_count

	; shared with the other drawing modules
	.globl gfx_cmd_block
	.globl gfx_cmd_commit
	.globl gfx_cmd_commit_short
	.globl _gfx_draw_shape
	.globl gfx_line_block
	.globl gfx_line_body
	.globl gfx_point_body
	.globl gfx_take_whco
	.globl gfx_check_xw
	.globl gfx_check_yh
	.globl gfx_shape_op
	.globl gfx_op_nibble
	.globl gfx_ret_zero
	.globl gfx_ret_einval
	.globl gfx_ret_enotsup
	.globl gfx_a_x
	.globl gfx_a_y
	.globl gfx_a_w
	.globl gfx_a_h
	.globl gfx_a_c
	.globl gfx_a_op
	.globl gfx_wait_ce

	.globl _gfx_v9958_port_cmd
	.globl _gfx_v9958_port_indirect

; The modes (enum gfx_mode).
GFX_MODE_G6             = 0
GFX_MODE_YJK            = 2

; The command codes and ARG bits (V9958 Technical Data Book, the drawing
; commands); the low nibble of LINE/PSET/LMMV is the logical operation
; (IMP 0, EOR 3).
GFX_CMD_HMMV            = 0xC0
GFX_CMD_HMMM            = 0xD0
GFX_CMD_LMMV            = 0x80
GFX_CMD_LINE            = 0x70
GFX_CMD_PSET            = 0x50
GFX_OP_EOR              = 0x03
GFX_ARG_MAJ             = 0x01
GFX_ARG_DIX             = 0x04
GFX_ARG_DIY             = 0x08

; The block's R#32..R#46 offsets.
GFX_BLK_SX              = 0
GFX_BLK_SY              = 2
GFX_BLK_DX              = 4
GFX_BLK_DY              = 6
GFX_BLK_NX              = 8
GFX_BLK_NY              = 10
GFX_BLK_CLR             = 12
GFX_BLK_ARG             = 13
GFX_BLK_CMD             = 14
GFX_BLK_BYTES           = 15

GFX_BATCH_ENTRIES       = 32
GFX_BATCH_DEPTH_MAX     = 8

; R#17's value (the indirect pointer at R#32) and its select byte.
GFX_R17_CMD_BASE        = 32
GFX_R17_SELECT          = 0x91

	.area _DATA

; The drawing state (C writes it: gfx.c, gfx_mode.c).
_gfx_draw_open::
	.ds 1			; 1 while the VIDEO handle is held
_gfx_draw_ready::
	.ds 1			; 1 while open with a mode set
_gfx_draw_mode::
	.ds 1			; enum gfx_mode
_gfx_draw_width::
	.ds 2			; the mode's width in dots
_gfx_draw_shape::
	.ds 1			; 2 G7, 1 G6 (a colour check), 0 neither
_gfx_batch_depth::
	.ds 1
_gfx_batch_count::
	.ds 1

; The arguments of the call in progress.
gfx_a_x::
	.ds 2
gfx_a_y::
	.ds 2
gfx_a_w::
	.ds 2
gfx_a_h::
	.ds 2
gfx_a_c::
	.ds 1			; the colour, then the op (one stack word)
gfx_a_op::
	.ds 1

; The rectangle's corners (gfx_rect's four lines).
gfx_r_x0:
	.ds 2
gfx_r_y0:
	.ds 2
gfx_r_x1:
	.ds 2
gfx_r_y1:
	.ds 2

; The block being built (byte 15: the first register's offset, 0 for
; R#32 or 4 for R#36) and the queue of ready blocks.
gfx_cmd_block::
	.ds 16
gfx_batch_queue:
	.ds 16 * GFX_BATCH_ENTRIES

	.area _CODE

; ---------------------------------------------------------------------------
; The results.
; ---------------------------------------------------------------------------
gfx_ret_zero::
	ld	de,#0
	ret
gfx_ret_einval::
	ld	de,#0xFFFE		; -KOS_EINVAL
	ret
gfx_ret_enotsup::
	ld	de,#0xFFF9		; -KOS_ENOTSUP
	ret

; ---------------------------------------------------------------------------
; gfx_take_whco: called first by an (a, b, c, d, colour, op) entry point:
; HL and DE are a and b, the stack holds c, d and the colour/op word.
; Stores them in gfx_a_x/y/w/h/c/op and pops the stack arguments.
; ---------------------------------------------------------------------------
gfx_take_whco::
	ld	(gfx_a_x),hl
	ld	(gfx_a_y),de
	pop	bc			; the return into the entry point
	pop	de			; the caller's return
	pop	hl
	ld	(gfx_a_w),hl
	pop	hl
	ld	(gfx_a_h),hl
	pop	hl
	ld	(gfx_a_c),hl		; the colour and the op
	push	de
	push	bc
	ret

; ---------------------------------------------------------------------------
; gfx_check_xw: HL = x, DE = width.  A = 0 inside the mode's width, 1 for a
; zero width, 2 outside (x >= width, or width > mode width - x).
; gfx_check_yh: HL = y, DE = height, the same against the 512-line space.
; ---------------------------------------------------------------------------
gfx_check_xw::
	ld	a,d
	or	e
	ld	a,#1
	ret	z
	push	de
	ex	de,hl
	ld	hl,(_gfx_draw_width)
	jr	gfx_check_span
gfx_check_yh::
	ld	a,d
	or	e
	ld	a,#1
	ret	z
	push	de
	ex	de,hl
	ld	hl,#512
gfx_check_span:
	or	a
	sbc	hl,de			; the room left of the start
	pop	de
	jr	c,gfx_check_bad
	jr	z,gfx_check_bad
	or	a
	sbc	hl,de
	jr	c,gfx_check_bad
	xor	a
	ret
gfx_check_bad:
	ld	a,#2
	ret

; ---------------------------------------------------------------------------
; gfx_shape_op: the op and the shape rule, then the open mode and the
; 16-colour colour: an unknown op is -KOS_EINVAL, YJK -KOS_ENOTSUP, no mode
; or a colour above 15 in the 16-colour mode -KOS_EINVAL.  Returns with
; carry clear when the call may go on; with carry set DE holds the result.
; ---------------------------------------------------------------------------
gfx_shape_op::
	ld	a,(gfx_a_op)
	cp	#2
	jr	nc,gfx_shape_einval
	ld	a,(_gfx_draw_shape)
	cp	#2
	jr	z,gfx_shape_g7
	dec	a
	jr	z,gfx_shape_g6
	; neither: YJK first, then no mode
	ld	a,(_gfx_draw_mode)
	cp	#GFX_MODE_YJK
	jr	z,gfx_shape_enotsup
	ld	a,(_gfx_draw_ready)
	or	a
	jr	z,gfx_shape_einval
	ld	a,(_gfx_draw_mode)
	or	a
	ret	nz			; carry clear
gfx_shape_g6:
	ld	a,(gfx_a_c)
	and	#0xF0
	ret	z			; carry clear
gfx_shape_einval:
	ld	de,#0xFFFE
	scf
	ret
gfx_shape_g7:
	or	a			; carry clear
	ret
gfx_shape_enotsup:
	ld	de,#0xFFF9
	scf
	ret

; gfx_op_nibble: A = the VDP logical-op nibble of gfx_a_op (IMP 0, EOR 3).
gfx_op_nibble::
	ld	a,(gfx_a_op)
	or	a
	ret	z
	ld	a,#GFX_OP_EOR
	ret

; ---------------------------------------------------------------------------
; The command commit and issue.
; ---------------------------------------------------------------------------

; gfx_wait_ce: wait until S#2.CE clears (status register 2 is selected).
; Leaves C = the command port.  Clobbers A.
gfx_wait_ce::
	ld	a,(_gfx_v9958_port_cmd)
	ld	c,a
gfx_wait_ce_loop:
	in	a,(c)
	rrca
	jr	c,gfx_wait_ce_loop
	ret

; gfx_cmd_commit: issue gfx_cmd_block now from R#32 (fifteen bytes), or
; queue it inside a batch.  gfx_cmd_commit_short: the same from R#36
; (eleven bytes, a command that reads no source).  Clobber A, BC, DE, HL.
gfx_cmd_commit_short::
	ld	a,#4
	ld	(gfx_cmd_block+15),a
	ld	a,(_gfx_batch_depth)
	or	a
	jr	nz,gfx_cmd_queue
	ld	hl,#gfx_cmd_block+4
	jr	gfx_issue_short
gfx_cmd_commit::
	xor	a
	ld	(gfx_cmd_block+15),a
	ld	a,(_gfx_batch_depth)
	or	a
	jr	nz,gfx_cmd_queue
	ld	hl,#gfx_cmd_block
; gfx_issue_full: HL = a block's R#32 byte; gfx_issue_short: HL = its
; R#36 byte.  Wait for the engine, select R#17, stream the bytes; the
; last (R#46) starts the command.
gfx_issue_full:
	call	gfx_wait_ce
	ld	a,#GFX_R17_CMD_BASE
	out	(c),a
	ld	a,#GFX_R17_SELECT
	out	(c),a
	ld	a,(_gfx_v9958_port_indirect)
	ld	c,a
	di
	outi
	outi
	outi
	outi
	jr	gfx_issue_11
gfx_issue_short:
	call	gfx_wait_ce
	ld	a,#GFX_R17_CMD_BASE + 4
	out	(c),a
	ld	a,#GFX_R17_SELECT
	out	(c),a
	ld	a,(_gfx_v9958_port_indirect)
	ld	c,a
	di
gfx_issue_11:
	outi
	outi
	outi
	outi
	outi
	outi
	outi
	outi
	outi
	outi
	outi
	ei
	ret

gfx_cmd_queue:
	ld	a,(_gfx_batch_count)
	cp	#GFX_BATCH_ENTRIES
	call	z,gfx_batch_run
	ld	a,(_gfx_batch_count)
	ld	l,a
	inc	a
	ld	(_gfx_batch_count),a
	ld	h,#0
	add	hl,hl
	add	hl,hl
	add	hl,hl
	add	hl,hl
	ld	de,#gfx_batch_queue
	add	hl,de
	ex	de,hl
	ld	hl,#gfx_cmd_block
	ld	bc,#16
	ldir
	ret

; gfx_batch_run: issue every queued block in order, then empty the queue.
gfx_batch_run:
	ld	a,(_gfx_batch_count)
	or	a
	ret	z
	ld	b,a
	ld	hl,#gfx_batch_queue
gfx_batch_run_loop:
	push	bc
	push	hl
	ld	de,#15
	add	hl,de
	ld	a,(hl)			; the first register's offset
	pop	hl
	push	hl
	or	a
	jr	z,gfx_batch_run_full
	ld	e,a
	ld	d,#0
	add	hl,de
	call	gfx_issue_short
	jr	gfx_batch_run_next
gfx_batch_run_full:
	call	gfx_issue_full
gfx_batch_run_next:
	pop	hl
	ld	de,#16
	add	hl,de
	pop	bc
	djnz	gfx_batch_run_loop
	xor	a
	ld	(_gfx_batch_count),a
	ret

; ---------------------------------------------------------------------------
; void gfx_begin(void); int gfx_end(void); int gfx_flush(void);
; ---------------------------------------------------------------------------
_gfx_begin::
	ld	a,(_gfx_draw_open)
	or	a
	ret	z
	ld	a,(_gfx_batch_depth)
	cp	#GFX_BATCH_DEPTH_MAX
	ret	nc
	inc	a
	ld	(_gfx_batch_depth),a
	ret

_gfx_end::
	ld	a,(_gfx_draw_open)
	or	a
	jp	z,gfx_ret_einval
	ld	a,(_gfx_batch_depth)
	or	a
	jp	z,gfx_ret_einval
	dec	a
	ld	(_gfx_batch_depth),a
	jp	nz,gfx_ret_zero
	call	gfx_batch_run
	jp	gfx_ret_zero

_gfx_flush::
	ld	a,(_gfx_draw_open)
	or	a
	jp	z,gfx_ret_einval
	call	gfx_batch_run
	jp	gfx_ret_zero

; ---------------------------------------------------------------------------
; int gfx_fill_rect(uint16_t x, uint16_t y, uint16_t width,
;                   uint16_t height, gfx_color_t color, enum gfx_op op);
;   HMMV when the op is COPY and x and width are byte-aligned, LMMV (the
;   per-dot logical fill) otherwise.
; int gfx_clear(gfx_color_t color);  A = color
; ---------------------------------------------------------------------------
_gfx_clear::
	ld	(gfx_a_c),a
	xor	a
	ld	(gfx_a_op),a
	ld	hl,#0
	ld	(gfx_a_x),hl
	ld	(gfx_a_y),hl
	ld	hl,(_gfx_draw_width)
	ld	(gfx_a_w),hl
	ld	hl,#212
	ld	(gfx_a_h),hl
	jr	gfx_fill_rect_body

_gfx_fill_rect::
	call	gfx_take_whco
gfx_fill_rect_body:
	ld	a,(_gfx_draw_ready)
	or	a
	jp	z,gfx_ret_einval
	ld	hl,(gfx_a_x)
	ld	de,(gfx_a_w)
	call	gfx_check_xw
	or	a
	jr	z,gfx_fill_rows
	dec	a
	jp	z,gfx_ret_zero
	jp	gfx_ret_einval
gfx_fill_rows:
	ld	hl,(gfx_a_y)
	ld	de,(gfx_a_h)
	call	gfx_check_yh
	or	a
	jr	z,gfx_fill_op
	dec	a
	jp	z,gfx_ret_zero
	jp	gfx_ret_einval
gfx_fill_op:
	ld	a,(gfx_a_op)
	cp	#2
	jp	nc,gfx_ret_einval
	ld	b,a			; B = the op
	ld	a,(_gfx_draw_mode)
	cp	#GFX_MODE_YJK
	jr	nz,gfx_fill_not_yjk
	; YJK: only an aligned COPY keeps the four-pixel J/K runs
	ld	a,b
	or	a
	jp	nz,gfx_ret_enotsup
	ld	a,(gfx_a_x)
	ld	c,a
	ld	a,(gfx_a_w)
	or	c
	and	#3
	jp	nz,gfx_ret_einval
	jr	gfx_fill_build
gfx_fill_not_yjk:
	or	a
	jr	nz,gfx_fill_build
	ld	a,(gfx_a_c)
	and	#0xF0
	jp	nz,gfx_ret_einval
gfx_fill_build:
	call	gfx_fill_block
	ld	a,b
	or	a
	jr	nz,gfx_fill_lmmv	; XOR is per dot
	ld	a,(_gfx_draw_mode)
	or	a
	jr	nz,gfx_fill_hmmv_byte
	ld	a,(gfx_a_x)
	ld	c,a
	ld	a,(gfx_a_w)
	or	c
	rrca
	jr	c,gfx_fill_lmmv		; an odd G6 edge is per dot
	ld	a,(gfx_a_c)		; G6: the pixel in both nibbles
	ld	c,a
	add	a,a
	add	a,a
	add	a,a
	add	a,a
	or	c
	jr	gfx_fill_hmmv
gfx_fill_hmmv_byte:
	ld	a,(gfx_a_c)
gfx_fill_hmmv:
	ld	(gfx_cmd_block+GFX_BLK_CLR),a
	ld	a,#GFX_CMD_HMMV
	jr	gfx_fill_commit
gfx_fill_lmmv:
	ld	a,(gfx_a_c)
	ld	(gfx_cmd_block+GFX_BLK_CLR),a
	call	gfx_op_nibble
	or	#GFX_CMD_LMMV
gfx_fill_commit:
	ld	(gfx_cmd_block+GFX_BLK_CMD),a
	call	gfx_cmd_commit_short
	jp	gfx_ret_zero

; gfx_fill_block: DX/DY/NX/NY from gfx_a_x/y/w/h, ARG zero (a fill reads
; no source: SX/SY are not issued).  Keeps BC.
gfx_fill_block:
	ld	hl,(gfx_a_x)
	ld	(gfx_cmd_block+GFX_BLK_DX),hl
	ld	hl,(gfx_a_y)
	ld	(gfx_cmd_block+GFX_BLK_DY),hl
	ld	hl,(gfx_a_w)
	ld	(gfx_cmd_block+GFX_BLK_NX),hl
	ld	hl,(gfx_a_h)
	ld	(gfx_cmd_block+GFX_BLK_NY),hl
	xor	a
	ld	(gfx_cmd_block+GFX_BLK_ARG),a
	ret

; ---------------------------------------------------------------------------
; int gfx_copy_rect(uint16_t src_x, uint16_t src_y, uint16_t dst_x,
;                   uint16_t dst_y, uint16_t width, uint16_t height);
;   One HMMM; when the source sits left of (or above) the destination the
;   overlapping rows (columns) copy from the far edge: DIX (DIY) and both
;   rectangles' far edges.
; ---------------------------------------------------------------------------
_gfx_copy_rect::
	ld	(gfx_r_x0),hl		; src_x
	ld	(gfx_r_y0),de		; src_y
	pop	bc
	pop	hl
	ld	(gfx_r_x1),hl		; dst_x
	pop	hl
	ld	(gfx_r_y1),hl		; dst_y
	pop	hl
	ld	(gfx_a_w),hl
	pop	hl
	ld	(gfx_a_h),hl
	push	bc
	ld	a,(_gfx_draw_ready)
	or	a
	jp	z,gfx_ret_einval
	ld	hl,(gfx_r_x0)
	ld	de,(gfx_a_w)
	call	gfx_check_xw
	or	a
	jr	z,gfx_copy_rows
	dec	a
	jp	z,gfx_ret_zero
	jp	gfx_ret_einval
gfx_copy_rows:
	ld	hl,(gfx_r_y0)
	ld	de,(gfx_a_h)
	call	gfx_check_yh
	or	a
	jr	z,gfx_copy_dst
	dec	a
	jp	z,gfx_ret_zero
	jp	gfx_ret_einval
gfx_copy_dst:
	ld	hl,(gfx_r_x1)
	ld	de,(gfx_a_w)
	call	gfx_check_xw
	or	a
	jp	nz,gfx_ret_einval
	ld	hl,(gfx_r_y1)
	ld	de,(gfx_a_h)
	call	gfx_check_yh
	or	a
	jp	nz,gfx_ret_einval
	ld	a,(gfx_r_x0)
	ld	c,a
	ld	a,(gfx_r_x1)
	or	c
	ld	c,a
	ld	a,(gfx_a_w)
	or	c
	ld	c,a			; C = src_x | dst_x | width (low byte)
	ld	a,(_gfx_draw_mode)
	or	a
	jr	nz,gfx_copy_not_g6
	ld	a,c
	rrca
	jp	c,gfx_ret_einval
	jr	gfx_copy_build
gfx_copy_not_g6:
	cp	#GFX_MODE_YJK
	jr	nz,gfx_copy_build
	ld	a,c
	and	#3
	jp	nz,gfx_ret_einval
gfx_copy_build:
	ld	b,#0			; B = ARG
	; X: forward, or the far edges with DIX
	ld	hl,(gfx_r_x0)
	ld	de,(gfx_r_x1)
	or	a
	sbc	hl,de
	jr	nc,gfx_copy_x_fwd
	ld	b,#GFX_ARG_DIX
	ld	de,(gfx_a_w)
	dec	de
	ld	hl,(gfx_r_x0)
	add	hl,de
	ld	(gfx_cmd_block+GFX_BLK_SX),hl
	ld	hl,(gfx_r_x1)
	add	hl,de
	ld	(gfx_cmd_block+GFX_BLK_DX),hl
	jr	gfx_copy_y
gfx_copy_x_fwd:
	ld	hl,(gfx_r_x0)
	ld	(gfx_cmd_block+GFX_BLK_SX),hl
	ld	hl,(gfx_r_x1)
	ld	(gfx_cmd_block+GFX_BLK_DX),hl
gfx_copy_y:
	ld	hl,(gfx_r_y0)
	ld	de,(gfx_r_y1)
	or	a
	sbc	hl,de
	jr	nc,gfx_copy_y_fwd
	ld	a,b
	or	#GFX_ARG_DIY
	ld	b,a
	ld	de,(gfx_a_h)
	dec	de
	ld	hl,(gfx_r_y0)
	add	hl,de
	ld	(gfx_cmd_block+GFX_BLK_SY),hl
	ld	hl,(gfx_r_y1)
	add	hl,de
	ld	(gfx_cmd_block+GFX_BLK_DY),hl
	jr	gfx_copy_rest
gfx_copy_y_fwd:
	ld	hl,(gfx_r_y0)
	ld	(gfx_cmd_block+GFX_BLK_SY),hl
	ld	hl,(gfx_r_y1)
	ld	(gfx_cmd_block+GFX_BLK_DY),hl
gfx_copy_rest:
	ld	hl,(gfx_a_w)
	ld	(gfx_cmd_block+GFX_BLK_NX),hl
	ld	hl,(gfx_a_h)
	ld	(gfx_cmd_block+GFX_BLK_NY),hl
	xor	a
	ld	(gfx_cmd_block+GFX_BLK_CLR),a
	ld	a,b
	ld	(gfx_cmd_block+GFX_BLK_ARG),a
	ld	a,#GFX_CMD_HMMM
	ld	(gfx_cmd_block+GFX_BLK_CMD),a
	call	gfx_cmd_commit
	jp	gfx_ret_zero

; ---------------------------------------------------------------------------
; int gfx_line(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
;              gfx_color_t color, enum gfx_op op);
;   One LINE from (x0, y0) to (x1, y1) inclusive.
; ---------------------------------------------------------------------------
_gfx_line::
	call	gfx_take_whco		; x0, y0, x1 (w), y1 (h)
gfx_line_body::
	call	gfx_shape_op
	ret	c
	ld	de,(_gfx_draw_width)
	ld	hl,(gfx_a_x)
	or	a
	sbc	hl,de
	jp	nc,gfx_ret_einval
	ld	hl,(gfx_a_w)
	or	a
	sbc	hl,de
	jp	nc,gfx_ret_einval
	ld	de,#512
	ld	hl,(gfx_a_y)
	or	a
	sbc	hl,de
	jp	nc,gfx_ret_einval
	ld	hl,(gfx_a_h)
	or	a
	sbc	hl,de
	jp	nc,gfx_ret_einval
	call	gfx_line_block
	call	gfx_cmd_commit_short
	jp	gfx_ret_zero

; gfx_line_block: the LINE block from gfx_a_x/y (the start), gfx_a_w/h
; (the end), gfx_a_c and gfx_a_op: DX/DY the start, Maj/Min the unsigned
; deltas with the longer on the major axis, ARG's MAJ/DIX/DIY from the
; delta signs (V9958 Technical Data Book, the LINE command: R#36-39 DX/DY,
; R#40-43 Maj/Min, R#44 CLR, R#45 ARG).
gfx_line_block::
	ld	c,#0			; C = ARG
	ld	hl,(gfx_a_w)
	ld	de,(gfx_a_x)
	or	a
	sbc	hl,de			; x1 - x0
	jr	nc,gfx_line_dx
	ex	de,hl
	ld	hl,#0
	or	a
	sbc	hl,de
	ld	c,#GFX_ARG_DIX
gfx_line_dx:
	push	hl			; |dx|
	ld	hl,(gfx_a_h)
	ld	de,(gfx_a_y)
	or	a
	sbc	hl,de			; y1 - y0
	jr	nc,gfx_line_dy
	ex	de,hl
	ld	hl,#0
	or	a
	sbc	hl,de
	set	3,c			; DIY
gfx_line_dy:
	pop	de			; DE = |dx|, HL = |dy|
	push	hl
	or	a
	sbc	hl,de			; |dy| - |dx|
	pop	hl
	jr	c,gfx_line_xmaj
	jr	z,gfx_line_xmaj
	set	0,c			; MAJ: Y is the long side
	ld	(gfx_cmd_block+GFX_BLK_NX),hl
	ld	(gfx_cmd_block+GFX_BLK_NY),de
	jr	gfx_line_rest
gfx_line_xmaj:
	ld	(gfx_cmd_block+GFX_BLK_NX),de
	ld	(gfx_cmd_block+GFX_BLK_NY),hl
gfx_line_rest:
	ld	a,c
	ld	(gfx_cmd_block+GFX_BLK_ARG),a
	ld	hl,(gfx_a_x)
	ld	(gfx_cmd_block+GFX_BLK_DX),hl
	ld	hl,(gfx_a_y)
	ld	(gfx_cmd_block+GFX_BLK_DY),hl
	ld	a,(gfx_a_c)
	ld	(gfx_cmd_block+GFX_BLK_CLR),a
	call	gfx_op_nibble
	or	#GFX_CMD_LINE
	ld	(gfx_cmd_block+GFX_BLK_CMD),a
	ret

; ---------------------------------------------------------------------------
; int gfx_point(uint16_t x, uint16_t y, gfx_color_t color, enum gfx_op op);
;   One PSET.
; ---------------------------------------------------------------------------
_gfx_point::
	ld	(gfx_a_x),hl
	ld	(gfx_a_y),de
	pop	bc
	pop	hl
	ld	(gfx_a_c),hl
	push	bc
gfx_point_body::
	call	gfx_shape_op
	ret	c
	ld	de,(_gfx_draw_width)
	ld	hl,(gfx_a_x)
	or	a
	sbc	hl,de
	jp	nc,gfx_ret_einval
	ld	de,#512
	ld	hl,(gfx_a_y)
	or	a
	sbc	hl,de
	jp	nc,gfx_ret_einval
	ld	hl,(gfx_a_x)
	ld	(gfx_cmd_block+GFX_BLK_DX),hl
	ld	hl,(gfx_a_y)
	ld	(gfx_cmd_block+GFX_BLK_DY),hl
	ld	(gfx_cmd_block+GFX_BLK_NX),hl
	ld	(gfx_cmd_block+GFX_BLK_NY),hl
	xor	a
	ld	(gfx_cmd_block+GFX_BLK_ARG),a
	ld	a,(gfx_a_c)
	ld	(gfx_cmd_block+GFX_BLK_CLR),a
	call	gfx_op_nibble
	or	#GFX_CMD_PSET
	ld	(gfx_cmd_block+GFX_BLK_CMD),a
	call	gfx_cmd_commit_short
	jp	gfx_ret_zero

; ---------------------------------------------------------------------------
; int gfx_rect(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
;              gfx_color_t color, enum gfx_op op);
;   Four LINE commands: the top and bottom edges, then the right and left
;   sides from bottom to top.
; ---------------------------------------------------------------------------
_gfx_rect::
	call	gfx_take_whco
	call	gfx_shape_op
	ret	c
	ld	hl,(gfx_a_x)
	ld	de,(gfx_a_w)
	call	gfx_check_xw
	or	a
	jr	z,gfx_rect_rows
	dec	a
	jp	z,gfx_ret_zero
	jp	gfx_ret_einval
gfx_rect_rows:
	ld	hl,(gfx_a_y)
	ld	de,(gfx_a_h)
	call	gfx_check_yh
	or	a
	jr	z,gfx_rect_draw
	dec	a
	jp	z,gfx_ret_zero
	jp	gfx_ret_einval
gfx_rect_draw:
	ld	hl,(gfx_a_x)
	ld	(gfx_r_x0),hl
	ld	de,(gfx_a_w)
	add	hl,de
	dec	hl
	ld	(gfx_r_x1),hl
	ld	hl,(gfx_a_y)
	ld	(gfx_r_y0),hl
	ld	de,(gfx_a_h)
	add	hl,de
	dec	hl
	ld	(gfx_r_y1),hl
	; (x0, y0) - (x1, y0)
	ld	hl,(gfx_r_x0)
	ld	(gfx_a_x),hl
	ld	hl,(gfx_r_y0)
	ld	(gfx_a_y),hl
	ld	hl,(gfx_r_x1)
	ld	(gfx_a_w),hl
	ld	hl,(gfx_r_y0)
	ld	(gfx_a_h),hl
	call	gfx_rect_edge
	; (x1, y0) - (x1, y1)
	ld	hl,(gfx_r_x1)
	ld	(gfx_a_x),hl
	ld	hl,(gfx_r_y0)
	ld	(gfx_a_y),hl
	ld	hl,(gfx_r_x1)
	ld	(gfx_a_w),hl
	ld	hl,(gfx_r_y1)
	ld	(gfx_a_h),hl
	call	gfx_rect_edge
	; (x1, y1) - (x0, y1)
	ld	hl,(gfx_r_x1)
	ld	(gfx_a_x),hl
	ld	hl,(gfx_r_y1)
	ld	(gfx_a_y),hl
	ld	hl,(gfx_r_x0)
	ld	(gfx_a_w),hl
	ld	hl,(gfx_r_y1)
	ld	(gfx_a_h),hl
	call	gfx_rect_edge
	; (x0, y1) - (x0, y0)
	ld	hl,(gfx_r_x0)
	ld	(gfx_a_x),hl
	ld	hl,(gfx_r_y1)
	ld	(gfx_a_y),hl
	ld	hl,(gfx_r_x0)
	ld	(gfx_a_w),hl
	ld	hl,(gfx_r_y0)
	ld	(gfx_a_h),hl
	call	gfx_rect_edge
	jp	gfx_ret_zero

gfx_rect_edge:
	call	gfx_line_block
	jp	gfx_cmd_commit_short
