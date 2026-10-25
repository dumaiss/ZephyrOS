; -----------------------------------------------------------------------------
; libgfx V9958 backend port I/O (user/sdk/gfx/src/backends/v9958).
;
; The port layer the C backend uses (v9958_gfx.c: the mode, the display,
; the page switch, the palette): the ports are the
; KVIDEO_OP_DIRECT_INFO record's, mirrored into the bytes below at
; gfx_open, so the OUT forms name the port through C.  The drawing
; itself - the command path, the shapes, text and the upload - is the
; gfx_*.asm modules beside this file.
;
; Call convention (sdcccall(1)):
;   void f(uint8_t value, uint8_t reg)    A = value, L = reg
;   void f(uint8_t value)                 A = value
;   void f(void)
;   void f(uint8_t r14, uint16_t offset)  A = r14, DE = offset
;   void f(const uint8_t *src, uint16_t length)  HL = src, DE = length
;
; The register write and the command wait mirror the proven driver
; primitives (drivers/v9958/v9958_io.asm v9958_io_reg /
; v9958_io_wait_command) without the driver's interrupt masking: the
; display init on release leaves the
; frame interrupt off and status register 2 selected, and the mode setup
; re-establishes both, so no interrupt can split a register pair or
; reselect the status register.
; -----------------------------------------------------------------------------

	.module gfx_v9958_io

	.globl _gfx_v9958_io_reg
	.globl _gfx_v9958_io_wait_command
	.globl _gfx_v9958_io_wait_vblank
	.globl _gfx_v9958_io_set_vram
	.globl _gfx_v9958_io_stream_vram
	.globl _gfx_v9958_io_palette_byte
	.globl _gfx_v9958_port_cmd
	.globl _gfx_v9958_port_indirect
	.globl _gfx_v9958_port_data
	.globl _gfx_v9958_port_palette
	.globl _gfx_font_transpose6
	.globl _gfx_font_transpose4

GFX_V9958_REG_SELECT    = 0x80
GFX_V9958_REG_R14       = 14
GFX_V9958_STATUS2_CE    = 0x01
GFX_V9958_STATUS2_VR    = 0x40
GFX_V9958_VRAM_ADDR_HI  = 0x40

	.area _DATA

; The ports the record reported (gfx_open loads them).
_gfx_v9958_port_cmd::
	.ds 1
_gfx_v9958_port_indirect::
	.ds 1
_gfx_v9958_port_data::
	.ds 1
_gfx_v9958_port_palette::
	.ds 1

	.area _CODE

; ---------------------------------------------------------------------------
; void gfx_v9958_io_reg(uint8_t value, uint8_t reg);  A=value, L=reg
;   One register write under native WAIT: value, register | 80h.  The
;   hardware holds the I/O cycle while the engine accepts the byte.
; ---------------------------------------------------------------------------
_gfx_v9958_io_reg::
	ld	h,a
	ld	a,(_gfx_v9958_port_cmd)
	ld	c,a
	ld	a,h
	out	(c),a
	ld	a,l
	or	#GFX_V9958_REG_SELECT
	out	(c),a
	ret

; ---------------------------------------------------------------------------
; void gfx_v9958_io_wait_command(void);
;   Block until S#2.CE clears: the status read resets the command
;   port's two-byte latch, so every register pair is written after the
;   wait, never across it.  A command that never completes is a
;   hardware fault.
; ---------------------------------------------------------------------------
_gfx_v9958_io_wait_command::
	ld	a,(_gfx_v9958_port_cmd)
	ld	c,a
gfx_v9958_wait_loop:
	in	a,(c)
	and	#GFX_V9958_STATUS2_CE
	jr	nz,gfx_v9958_wait_loop
	ret

; ---------------------------------------------------------------------------
; void gfx_v9958_io_wait_vblank(void);
;   Block until the start of the next vertical retrace: first wait for
;   S#2.VR to clear (leaving any current retrace), then for it to rise.
;   A call inside a retrace therefore waits for the following frame.
; ---------------------------------------------------------------------------
_gfx_v9958_io_wait_vblank::
	ld	a,(_gfx_v9958_port_cmd)
	ld	c,a
gfx_v9958_vblank_clear:
	in	a,(c)
	and	#GFX_V9958_STATUS2_VR
	jr	nz,gfx_v9958_vblank_clear
gfx_v9958_vblank_set:
	in	a,(c)
	and	#GFX_V9958_STATUS2_VR
	jr	z,gfx_v9958_vblank_set
	ret

; ---------------------------------------------------------------------------
; void gfx_v9958_io_set_vram(uint8_t r14, uint16_t offset);  A=r14, DE=offset
;   Wait for the engine, select R#14 = A16..A14, then the two
;   command-port address bytes; the data port's following writes land at
;   the address and auto-increment.  The caller computes r14 and offset
;   from the pixel row (gfx_upload.c).
; ---------------------------------------------------------------------------
_gfx_v9958_io_set_vram::
	push	af			; the R#14 value survives the wait
	call	_gfx_v9958_io_wait_command
	pop	af
	ld	l,#GFX_V9958_REG_R14
	call	_gfx_v9958_io_reg
	ld	a,(_gfx_v9958_port_cmd)
	ld	c,a
	ld	a,e
	out	(c),a			; the address's low byte
	ld	a,d
	and	#0x3F
	or	#GFX_V9958_VRAM_ADDR_HI
	out	(c),a			; its high byte, write mode
	ret

; ---------------------------------------------------------------------------
; void gfx_v9958_io_stream_vram(const uint8_t *src, uint16_t length);
;   HL = src, DE = length.  The upload's byte stream: `length` bytes
;   through the data port in 256-byte blocks, the hardware WAIT pacing
;   every OUT.
; ---------------------------------------------------------------------------
_gfx_v9958_io_stream_vram::
	ex	de,hl			; HL = length, DE = src
	ld	a,(_gfx_v9958_port_data)
	ld	c,a
gfx_v9958_vram_loop:
	ld	a,h
	or	a
	jr	nz,gfx_v9958_vram_256
	ld	a,l
	or	a
	ret	z			; done
	ld	b,l
	ld	l,#0x00		; the remainder is this block
	jr	gfx_v9958_vram_run
gfx_v9958_vram_256:
	dec	h
	ld	b,#0x00		; 256 bytes
gfx_v9958_vram_run:
	ex	de,hl			; HL = src, DE = remainder
gfx_v9958_vram_byte:
	ld	a,(hl)
	out	(c),a
	inc	hl
	djnz	gfx_v9958_vram_byte
	ex	de,hl			; HL = remainder, DE = src
	jr	gfx_v9958_vram_loop

; ---------------------------------------------------------------------------
; void gfx_v9958_io_palette_byte(uint8_t value);  A=value
;   One palette-port byte; the hardware WAIT paces it, and the palette
;   pointer auto-advances after every pair.
; ---------------------------------------------------------------------------
_gfx_v9958_io_palette_byte::
	push	af
	ld	a,(_gfx_v9958_port_palette)
	ld	c,a
	pop	af
	out	(c),a
	ret

; ---------------------------------------------------------------------------
; void gfx_font_transpose6(uint8_t page, const uint8_t *src);  A, DE
; void gfx_font_transpose4(uint8_t page, const uint8_t *src);  A, DE
;   Lay a const font (256 glyphs at an 8-byte stride) out row-major for
;   the text loop (gfx_text.asm): row r of glyph c goes to page + r,
;   offset c.  The 6x8's pixels (bits 7..2) are kept as they are (the
;   pattern times 4); the 4x6's (bits 7..4) become the pattern times 2.
; ---------------------------------------------------------------------------
_gfx_font_transpose6::
	ex	de,hl			; HL = the glyphs
	ld	c,a			; C = the first row page
	ld	e,#0
	ld	b,e			; 256 glyphs
gfx_transpose6_glyph:
	ld	d,c
	ld	a,(hl)
	inc	hl
	and	#0xFC
	ld	(de),a
	inc	d
	ld	a,(hl)
	inc	hl
	and	#0xFC
	ld	(de),a
	inc	d
	ld	a,(hl)
	inc	hl
	and	#0xFC
	ld	(de),a
	inc	d
	ld	a,(hl)
	inc	hl
	and	#0xFC
	ld	(de),a
	inc	d
	ld	a,(hl)
	inc	hl
	and	#0xFC
	ld	(de),a
	inc	d
	ld	a,(hl)
	inc	hl
	and	#0xFC
	ld	(de),a
	inc	d
	ld	a,(hl)
	inc	hl
	and	#0xFC
	ld	(de),a
	inc	d
	ld	a,(hl)
	inc	hl
	and	#0xFC
	ld	(de),a
	inc	d
	inc	e
	djnz	gfx_transpose6_glyph
	ret

_gfx_font_transpose4::
	ex	de,hl
	ld	c,a
	ld	e,#0
	ld	b,e
gfx_transpose4_glyph:
	ld	d,c
	ld	a,(hl)
	inc	hl
	rrca
	rrca
	rrca
	and	#0x1E
	ld	(de),a
	inc	d
	ld	a,(hl)
	inc	hl
	rrca
	rrca
	rrca
	and	#0x1E
	ld	(de),a
	inc	d
	ld	a,(hl)
	inc	hl
	rrca
	rrca
	rrca
	and	#0x1E
	ld	(de),a
	inc	d
	ld	a,(hl)
	inc	hl
	rrca
	rrca
	rrca
	and	#0x1E
	ld	(de),a
	inc	d
	ld	a,(hl)
	inc	hl
	rrca
	rrca
	rrca
	and	#0x1E
	ld	(de),a
	inc	d
	ld	a,(hl)
	inc	hl
	rrca
	rrca
	rrca
	and	#0x1E
	ld	(de),a
	inc	d
	inc	hl			; the two pad rows
	inc	hl
	inc	e
	djnz	gfx_transpose4_glyph
	ret
