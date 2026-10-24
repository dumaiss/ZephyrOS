; -----------------------------------------------------------------------------
; libgfx V9958 backend port I/O (user/sdk/gfx/src/backends/v9958).
;
; Every port access the backend makes lives here; v9958_gfx.c owns the
; sequencing and the register values.  The ports are the
; KVIDEO_OP_DIRECT_INFO record's, mirrored into the bytes below at mode
; setup, so the OUT forms name the port through C.
;
; Call convention (sdcccall(0), the default of this project's SDCC):
;   void f(uint8_t value, uint8_t reg)    A = value, L = reg
;   void f(uint8_t value)                 A = value
;   void f(void)
;   void f(const uint8_t *cmd)            HL = cmd
;   void f(uint8_t r14, uint16_t offset)  A = r14, HL = offset
;   void f(const uint8_t *src, uint16_t length)  HL = src, DE = length
;
; The register write, the command wait and the command-engine stream
; mirror the proven driver primitives (drivers/v9958/v9958_io.asm
; v9958_io_reg / v9958_io_wait_command / v9958_start_command) without
; the driver's interrupt masking: the display init on release leaves the
; frame interrupt off and status register 2 selected, and the mode setup
; re-establishes both, so no interrupt can split a register pair or
; reselect the status register.
; -----------------------------------------------------------------------------

	.module gfx_v9958_io

	.globl _gfx_v9958_io_reg
	.globl _gfx_v9958_io_wait_command
	.globl _gfx_v9958_io_start_command
	.globl _gfx_v9958_io_set_vram
	.globl _gfx_v9958_io_stream_vram
	.globl _gfx_v9958_io_palette_byte
	.globl _gfx_v9958_port_cmd
	.globl _gfx_v9958_port_indirect
	.globl _gfx_v9958_port_data
	.globl _gfx_v9958_port_palette

; The command engine: R#17 selects R#32..R#46, the last write starts the
; command.  Status register 2's bit 0 is the engine's busy flag.
GFX_V9958_REG_SELECT    = 0x80
GFX_V9958_REG_R14       = 14
GFX_V9958_REG_R17       = 17
GFX_V9958_CMD_BASE      = 32
GFX_V9958_CMD_COUNT     = 15
GFX_V9958_STATUS2_CE    = 0x01
GFX_V9958_VRAM_ADDR_HI  = 0x40

	.area _DATA

; The ports the record reported (v9958_gfx.c writes them at mode setup).
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
; void gfx_v9958_io_start_command(const uint8_t *cmd);  HL=cmd
;   Wait for the engine, select R#17 = 32 and stream the 15-byte
;   command block through the indirect port; the last byte (R#46)
;   starts the command.  The caller waits for its completion.
; ---------------------------------------------------------------------------
_gfx_v9958_io_start_command::
	push	hl
	call	_gfx_v9958_io_wait_command
	ld	a,#GFX_V9958_CMD_BASE
	ld	l,#GFX_V9958_REG_R17
	call	_gfx_v9958_io_reg
	pop	hl
	ld	a,(_gfx_v9958_port_indirect)
	ld	c,a
	ld	b,#GFX_V9958_CMD_COUNT
gfx_v9958_start_loop:
	ld	a,(hl)
	out	(c),a
	inc	hl
	djnz	gfx_v9958_start_loop
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
