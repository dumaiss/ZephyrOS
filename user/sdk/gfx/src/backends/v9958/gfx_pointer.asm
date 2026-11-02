; -----------------------------------------------------------------------------
; libgfx: the pointer's show, move and hide (docs/gfx-sdk.md section 15.2).
;
; int gfx_pointer_show(int x, int y);   HL = x, DE = y
; int gfx_pointer_hide(void);
;
; Slot 0 of a 16x16 sprite-mode-2 table in page 1's lines 496-511
; (gfx_pointer.c installs it at the first show after a mode set).  The
; V9958 draws a sprite one line below its SAT Y, so Y is y - 1, and a Y
; of D8h would end the table's evaluation: it is drawn one line higher
; (D7h).  In GRAPHIC 6 a sprite X unit is two pixels (X = x / 2), in
; GRAPHIC 7 one (X = x).  The SAT is written only when the entry
; changes, after the command engine, and never inside an upload (its
; stream keeps the VRAM address across calls): the move then waits for
; the next show.  The first show after a hide sets SI (R#1) and clears
; SPD (R#8) after the SAT; the hide sets SPD first, so a program
; without a shown pointer keeps the engine's sprites-off speed.
;
; Call convention (sdcccall(1)): HL and DE, the result in DE; IX is
; preserved.
; -----------------------------------------------------------------------------

	.module gfx_pointer

	.globl _gfx_pointer_show
	.globl _gfx_pointer_hide

	.globl _gfx_pointer_install
	.globl _gfx_pointer_flags
	.globl _gfx_pointer_sat
	.globl _gfx_upload_active
	.globl _gfx_v9958_r1
	.globl _gfx_v9958_port_cmd
	.globl _gfx_v9958_port_data
	.globl _gfx_v9958_io_reg
	.globl _gfx_v9958_io_wait_command
	.globl _gfx_draw_ready
	.globl _gfx_draw_width

GFX_HEIGHT              = 212
GFX_SAT_TERMINATOR      = 0xD8
GFX_POINTER_INSTALLED   = 0
GFX_POINTER_SHOWN       = 1
GFX_R1_SI               = 0x02
GFX_R8_SPRITES_ON       = 0x08	; 64Kx4 DRAM, SPD clear
GFX_R8_SPRITES_OFF      = 0x0A	; 64Kx4 DRAM, SPD set
; The SAT's address 1F200h: R#14 = A16..A14, then A7..A0 and A13..A8
; with the write flag.
GFX_SAT_R14             = 0x07
GFX_SAT_LOW             = 0x00
GFX_SAT_HIGH            = 0x72
GFX_REG_SELECT_R14      = 0x8E

; The lowest visible Y (line 211, one up) stays clear of the terminator:
; the build fails if the screen grows into it.  The move path's D8h
; check guards it at run time.
	.ifge	(GFX_HEIGHT - 2) - GFX_SAT_TERMINATOR
	.error	1			; a visible Y reaches the terminator
	.endif

	.area _CODE

gfx_pointer_einval:
	ld	de,#0xFFFE		; -KOS_EINVAL
	ret

; ---------------------------------------------------------------------------
_gfx_pointer_show::
	ld	a,(_gfx_draw_ready)
	or	a
	jr	z,gfx_pointer_einval
	; x: 0 .. width-1 (a negative x is a large unsigned one)
	ld	bc,(_gfx_draw_width)
	ld	a,l
	sub	c
	ld	a,h
	sbc	a,b
	jr	nc,gfx_pointer_einval
	; y: 0 .. 211
	ld	a,d
	or	a
	jr	nz,gfx_pointer_einval
	ld	a,e
	cp	#GFX_HEIGHT
	jr	nc,gfx_pointer_einval
	; D = Y: one line up, never the terminator
	dec	a
	cp	#GFX_SAT_TERMINATOR
	jr	nz,gfx_pointer_y
	dec	a
gfx_pointer_y:
	ld	d,a
	; E = X: x / 2 in GRAPHIC 6 (the 512-dot width), x in GRAPHIC 7
	ld	a,b
	dec	a			; width 100h: Z
	jr	z,gfx_pointer_x
	srl	h
	rr	l
gfx_pointer_x:
	ld	e,l
	ld	a,(_gfx_pointer_flags)
	bit	GFX_POINTER_INSTALLED,a
	jr	nz,gfx_pointer_place
	push	de
	call	_gfx_pointer_install
	pop	de
gfx_pointer_place:
	ld	hl,(_gfx_pointer_sat)	; L = Y, H = X as last written
	ld	a,l
	cp	d
	jr	nz,gfx_pointer_write
	ld	a,h
	cp	e
	jr	z,gfx_pointer_on
gfx_pointer_write:
	ld	a,(_gfx_upload_active)
	or	a
	jr	nz,gfx_pointer_on	; the stream owns the address
	ld	l,d
	ld	h,e
	ld	(_gfx_pointer_sat),hl
	call	_gfx_v9958_io_wait_command	; C = the command port
	ld	a,#GFX_SAT_R14
	out	(c),a
	ld	a,#GFX_REG_SELECT_R14
	out	(c),a
	ld	a,#GFX_SAT_LOW
	out	(c),a
	ld	a,#GFX_SAT_HIGH
	out	(c),a
	ld	a,(_gfx_v9958_port_data)
	ld	c,a
	out	(c),d
	out	(c),e
gfx_pointer_on:
	ld	a,(_gfx_pointer_flags)
	bit	GFX_POINTER_SHOWN,a
	jr	nz,gfx_pointer_zero
	set	GFX_POINTER_SHOWN,a
	ld	(_gfx_pointer_flags),a
	ld	a,(_gfx_v9958_r1)
	or	#GFX_R1_SI
	ld	(_gfx_v9958_r1),a
	ld	l,#1
	call	_gfx_v9958_io_reg
	ld	a,#GFX_R8_SPRITES_ON
	ld	l,#8
	call	_gfx_v9958_io_reg
gfx_pointer_zero:
	ld	de,#0
	ret

; ---------------------------------------------------------------------------
_gfx_pointer_hide::
	ld	a,(_gfx_draw_ready)
	or	a
	jp	z,gfx_pointer_einval
	ld	a,(_gfx_pointer_flags)
	bit	GFX_POINTER_SHOWN,a
	jr	z,gfx_pointer_zero
	res	GFX_POINTER_SHOWN,a
	ld	(_gfx_pointer_flags),a
	ld	a,#GFX_R8_SPRITES_OFF
	ld	l,#8
	call	_gfx_v9958_io_reg
	ld	a,(_gfx_v9958_r1)
	and	#0xFD			; SI clear
	ld	(_gfx_v9958_r1),a
	ld	l,#1
	call	_gfx_v9958_io_reg
	jr	gfx_pointer_zero
