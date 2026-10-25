; -----------------------------------------------------------------------------
; libgfx V9958 backend port I/O (user/sdk/gfx/src/backends/v9958).
;
; Every port access the backend makes lives here; v9958_gfx.c owns the
; sequencing and the register values.  The ports are the
; KVIDEO_OP_DIRECT_INFO record's, mirrored into the bytes below at mode
; setup, so the OUT forms name the port through C.
;
; The command pipeline is here too: one builder per command shape
; (HMMV/HMMM/LINE/PSET/LMMV) turns a `struct gfx_cmd` record into the
; 15-byte R#32..R#46 block and issues it with one R#17 select and one
; OUTI/OTIR, then the caller waits for the engine.  Every shape and
; every glyph pays the same per-command cost.  The block is built in a
; shared 15-byte scratch area (the library runs one command at a
; time), never on the stack.
;
; Call convention (sdcccall(0), the default of this project's SDCC):
;   void f(uint8_t value, uint8_t reg)    A = value, L = reg
;   void f(uint8_t value)                 A = value
;   void f(void)
;   void f(const uint8_t *cmd)            HL = cmd
;   void f(const struct gfx_cmd *record)  HL = record
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
	.globl _gfx_v9958_io_wait_vblank
	.globl _gfx_v9958_io_start_command
	.globl _gfx_v9958_io_run_hmmv
	.globl _gfx_v9958_io_run_hmmm
	.globl _gfx_v9958_io_run_line
	.globl _gfx_v9958_io_run_pset
	.globl _gfx_v9958_io_run_lmmv
	.globl _gfx_v9958_io_run_lmmm
	.globl _gfx_v9958_io_set_vram
	.globl _gfx_v9958_io_stream_vram
	.globl _gfx_v9958_io_palette_byte
	.globl _gfx_v9958_port_cmd
	.globl _gfx_v9958_port_indirect
	.globl _gfx_v9958_port_data
	.globl _gfx_v9958_port_palette
	.globl _gfx_v9958_g6

; The command engine: R#17 selects R#32..R#46, the last write starts the
; command.  Status register 2's bit 0 is the engine's busy flag.
GFX_V9958_REG_SELECT    = 0x80
GFX_V9958_REG_R14       = 14
GFX_V9958_REG_R17       = 17
GFX_V9958_CMD_BASE      = 32
GFX_V9958_CMD_COUNT     = 15
GFX_V9958_STATUS2_CE    = 0x01
GFX_V9958_STATUS2_VR    = 0x40
GFX_V9958_VRAM_ADDR_HI  = 0x40

; The command codes and argument bits (V9958 Technical Data Book, the
; drawing commands): the low nibble of LINE/PSET/LMMV is the logical
; operation (IMP 0, EOR 3).
GFX_V9958_CMD_HMMV      = 0xC0
GFX_V9958_CMD_HMMM      = 0xD0
GFX_V9958_CMD_LINE      = 0x70
GFX_V9958_CMD_PSET      = 0x50
GFX_V9958_CMD_LMMV      = 0x80
GFX_V9958_CMD_LMMM      = 0x90
GFX_V9958_ARG_MAJ       = 0x01
GFX_V9958_ARG_DIX       = 0x04
GFX_V9958_ARG_DIY       = 0x08

; The 15-byte command block's R#32..R#46 offset (the byte each
; register writes through R#17's auto-increment pointer).
GFX_V9958_BLOCK_DX      = 4
GFX_V9958_BLOCK_DY      = 6
GFX_V9958_BLOCK_NX      = 8
GFX_V9958_BLOCK_NY      = 10
GFX_V9958_BLOCK_CLR     = 12
GFX_V9958_BLOCK_ARG     = 13

; The gfx_cmd record's field offsets.
GFX_CMD_FLAGS           = 1
GFX_CMD_SX              = 2
GFX_CMD_SY              = 4
GFX_CMD_DX              = 6
GFX_CMD_DY              = 8
GFX_CMD_WIDTH           = 10
GFX_CMD_HEIGHT          = 12
GFX_CMD_COLOR           = 14
GFX_CMD_RESERVED        = 15

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

; The mode's pixel packing: 1 in GRAPHIC 6 (two 4-bit pixels per byte),
; 0 in GRAPHIC 7 (one 8-bit pixel).  v9958_gfx.c sets it per batch.
_gfx_v9958_g6::
	.ds 1

; The command block the builders fill (one command runs at a
; time).  Byte 15 is a spare, outside the streamed R#32..R#46 range.
gfx_v9958_block:
	.ds GFX_V9958_CMD_COUNT + 1

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
; The shared issue tail: the builder leaves the block in the scratch
; and HL at the block's code byte (the 15th); this selects R#17 = 32
; once and streams the block with one OUTI loop, the last byte
; starting the command.  Clobbers A, BC, HL.
; ---------------------------------------------------------------------------
gfx_v9958_issue:
	call	_gfx_v9958_io_wait_command
	ld	a,#GFX_V9958_CMD_BASE
	ld	l,#GFX_V9958_REG_R17
	call	_gfx_v9958_io_reg
	ld	a,(_gfx_v9958_port_indirect)
	ld	c,a
	ld	b,#GFX_V9958_CMD_COUNT
	ld	hl,#gfx_v9958_block
	di
gfx_v9958_issue_otir:
	outi
	jr	nz,gfx_v9958_issue_otir
	ei
	ret

; ---------------------------------------------------------------------------
; void gfx_v9958_io_run_hmmv(const struct gfx_cmd *record);  HL=record
;   HMMV (the byte fill): DX/DY, NX/NY and CLR from the record; the
;   16-colour mode packs two 4-bit pixels per byte, GRAPHIC 7 takes
;   the byte as is.
; ---------------------------------------------------------------------------
_gfx_v9958_io_run_hmmv::
	push	ix
	push	hl
	pop	ix			; IX = the record
	ld	a,(_gfx_v9958_g6)
	or	a
	jr	nz,gfx_v9958_hmmv_g6
	ld	a,GFX_CMD_COLOR(ix)
	jr	gfx_v9958_hmmv_run
gfx_v9958_hmmv_g6:
	ld	a,GFX_CMD_COLOR(ix)
	ld	l,a
	add	a,a
	add	a,a
	add	a,a
	add	a,a
	or	l			; the pixel in both nibbles
gfx_v9958_hmmv_run:
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_CLR),a
	ld	a,#GFX_V9958_CMD_HMMV
	ld	(#gfx_v9958_block+14),a
	jr	gfx_v9958_fill_run

; ---------------------------------------------------------------------------
; void gfx_v9958_io_run_lmmv(const struct gfx_cmd *record);  HL=record
;   LMMV (the per-dot logical fill): DX/DY, NX/NY and CLR; the code
;   carries the operation nibble (IMP or EOR).
; ---------------------------------------------------------------------------
_gfx_v9958_io_run_lmmv::
	push	ix
	push	hl
	pop	ix			; IX = the record
	ld	a,GFX_CMD_COLOR(ix)
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_CLR),a
	ld	a,GFX_CMD_FLAGS(ix)	; the operation nibble
	or	#GFX_V9958_CMD_LMMV
	ld	(#gfx_v9958_block+14),a
; the shared fill block: DX/DY and NX/NY from the record, zero the
; rest, then the command code the caller left in the record's flags.
gfx_v9958_fill_run:
	ld	l,GFX_CMD_DX(ix)
	ld	h,GFX_CMD_DX+1(ix)
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_DX),hl
	ld	l,GFX_CMD_DY(ix)
	ld	h,GFX_CMD_DY+1(ix)
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_DY),hl
	ld	l,GFX_CMD_WIDTH(ix)
	ld	h,GFX_CMD_WIDTH+1(ix)
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_NX),hl
	ld	l,GFX_CMD_HEIGHT(ix)
	ld	h,GFX_CMD_HEIGHT+1(ix)
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_NY),hl
	ld	hl,#0
	ld	(#gfx_v9958_block+0),hl
	ld	(#gfx_v9958_block+2),hl
	ld	a,#0x00
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_ARG),a
	jp	gfx_v9958_issue_common

; ---------------------------------------------------------------------------
; void gfx_v9958_io_run_lmmm(const struct gfx_cmd *record);  HL=record
;   LMMM (the logical copy, one transparent glyph): SX/SY, DX/DY and
;   NX/NY from the record, CLR zero, ARG from the record's reserved
;   byte (DIX/DIY, the C side moved the far edges), the code carries
;   the operation nibble (TIMP 8).
; ---------------------------------------------------------------------------
_gfx_v9958_io_run_lmmm::
	push	ix
	push	hl
	pop	ix			; IX = the record
	ld	l,GFX_CMD_SX(ix)
	ld	h,GFX_CMD_SX+1(ix)
	ld	(#gfx_v9958_block+0),hl
	ld	l,GFX_CMD_SY(ix)
	ld	h,GFX_CMD_SY+1(ix)
	ld	(#gfx_v9958_block+2),hl
	ld	l,GFX_CMD_DX(ix)
	ld	h,GFX_CMD_DX+1(ix)
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_DX),hl
	ld	l,GFX_CMD_DY(ix)
	ld	h,GFX_CMD_DY+1(ix)
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_DY),hl
	ld	l,GFX_CMD_WIDTH(ix)
	ld	h,GFX_CMD_WIDTH+1(ix)
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_NX),hl
	ld	l,GFX_CMD_HEIGHT(ix)
	ld	h,GFX_CMD_HEIGHT+1(ix)
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_NY),hl
	ld	a,#0x00
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_CLR),a
	ld	a,GFX_CMD_RESERVED(ix)	; DIX/DIY from the C side
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_ARG),a
	ld	a,GFX_CMD_FLAGS(ix)	; the operation nibble (TIMP)
	or	#GFX_V9958_CMD_LMMM
	ld	(#gfx_v9958_block+14),a
	jp	gfx_v9958_issue_common

; ---------------------------------------------------------------------------
; void gfx_v9958_io_run_hmmm(const struct gfx_cmd *record);  HL=record
;   HMMM: SX/SY/DX/DY/NX/NY from the (direction-adjusted) record, ARG
;   from flags' DIX/DIY.  The C side already moved the far edges and
;   the direction bits.
; ---------------------------------------------------------------------------
_gfx_v9958_io_run_hmmm::
	push	ix
	push	hl
	pop	ix			; IX = the record
	ld	l,GFX_CMD_SX(ix)
	ld	h,GFX_CMD_SX+1(ix)
	ld	(#gfx_v9958_block+0),hl
	ld	l,GFX_CMD_SY(ix)
	ld	h,GFX_CMD_SY+1(ix)
	ld	(#gfx_v9958_block+2),hl
	ld	l,GFX_CMD_DX(ix)
	ld	h,GFX_CMD_DX+1(ix)
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_DX),hl
	ld	l,GFX_CMD_DY(ix)
	ld	h,GFX_CMD_DY+1(ix)
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_DY),hl
	ld	l,GFX_CMD_WIDTH(ix)
	ld	h,GFX_CMD_WIDTH+1(ix)
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_NX),hl
	ld	l,GFX_CMD_HEIGHT(ix)
	ld	h,GFX_CMD_HEIGHT+1(ix)
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_NY),hl
	ld	hl,#0
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_CLR),hl
	; ARG: the record's DIX/DIY flags
	ld	a,GFX_CMD_FLAGS(ix)
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_ARG),a
	ld	hl,#gfx_v9958_block+14
	ld	a,#GFX_V9958_CMD_HMMM
	ld	(hl),a
	jp	gfx_v9958_issue_common

; ---------------------------------------------------------------------------
; void gfx_v9958_io_run_line(const struct gfx_cmd *record);  HL=record
;   LINE: the record's dx/dy is the start, sx/sy the end; Maj/Min are
;   the unsigned deltas with the longer one on the major axis, and
;   ARG's MAJ/DIX/DIY from the delta signs (V9958 Technical Data Book,
;   the LINE command: R#36-39 DX/DY, R#40-43 Maj/Min, R#44 CLR, R#45
;   ARG).
; ---------------------------------------------------------------------------
_gfx_v9958_io_run_line::
	push	ix
	push	hl
	pop	ix			; IX = the record
	ld	e,#0x00
	ld	l,GFX_CMD_SX(ix)	; x1
	ld	h,GFX_CMD_SX+1(ix)
	ld	a,l
	sub	GFX_CMD_DX(ix)		; x1 - x0
	ld	l,a
	ld	a,h
	sbc	a,GFX_CMD_DX+1(ix)
	ld	h,a			; HL = x1 - x0 (signed)
	jr	c,gfx_v9958_line_negx	; negative: DIX
	jr	nz,gfx_v9958_line_dx1
	ld	a,l
	or	a
	jr	nz,gfx_v9958_line_dx1
	jr	gfx_v9958_line_zero_dx
gfx_v9958_line_negx:
	ld	a,l
	cpl
	ld	l,a
	ld	a,h
	cpl
	ld	h,a
	inc	hl			; HL = |x1 - x0|
	ld	e,#GFX_V9958_ARG_DIX
	jr	gfx_v9958_line_dx1
gfx_v9958_line_zero_dx:
	ld	hl,#0
gfx_v9958_line_dx1:
	push	hl			; |dx| kept
	ld	l,GFX_CMD_SY(ix)	; y1
	ld	h,GFX_CMD_SY+1(ix)
	ld	a,l
	sub	GFX_CMD_DY(ix)		; y1 - y0
	ld	l,a
	ld	a,h
	sbc	a,GFX_CMD_DY+1(ix)
	ld	h,a			; HL = y1 - y0 (signed)
	jr	c,gfx_v9958_line_negy	; negative: DIY
	jr	nz,gfx_v9958_line_dy1
	ld	a,l
	or	a
	jr	nz,gfx_v9958_line_dy1
	jr	gfx_v9958_line_zero_dy
gfx_v9958_line_negy:
	ld	a,l
	cpl
	ld	l,a
	ld	a,h
	cpl
	ld	h,a
	inc	hl			; HL = |y1 - y0|
	ld	a,e
	or	#GFX_V9958_ARG_DIY
	ld	e,a
	jr	gfx_v9958_line_dy1
gfx_v9958_line_zero_dy:
	ld	hl,#0
gfx_v9958_line_dy1:
	; The direction bits are done: keep them in the block's spare byte
	; (byte 15, outside the streamed range) - the pop below restores
	; |dx| into DE, overwriting E.
	ld	a,e
	ld	(#gfx_v9958_block+GFX_V9958_CMD_COUNT),a
	pop	de			; DE = |dx|
	; the longer delta is the major: compare |dx| and |dy|
	ld	b,h
	ld	c,l			; BC = |dy|
	ld	l,e
	ld	h,d
	or	a
	sbc	hl,bc			; HL = |dx| - |dy|
	jr	nc,gfx_v9958_line_xmaj
	; Y is the major: Maj = |dy|, Min = |dx|, MAJ set
	ld	a,(#gfx_v9958_block+GFX_V9958_CMD_COUNT)
	or	#GFX_V9958_ARG_MAJ
	ld	(#gfx_v9958_block+GFX_V9958_CMD_COUNT),a
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_NX),bc
	ld	l,e
	ld	h,d
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_NY),hl
	jr	gfx_v9958_line_block
gfx_v9958_line_xmaj:
	; X is the major: Maj = |dx|, Min = |dy|
	ld	l,e
	ld	h,d
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_NX),hl
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_NY),bc
gfx_v9958_line_block:
	ld	l,GFX_CMD_DX(ix)
	ld	h,GFX_CMD_DX+1(ix)
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_DX),hl
	ld	l,GFX_CMD_DY(ix)
	ld	h,GFX_CMD_DY+1(ix)
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_DY),hl
	ld	hl,#0
	ld	(#gfx_v9958_block+0),hl
	ld	(#gfx_v9958_block+2),hl
	ld	a,(#gfx_v9958_block+GFX_V9958_CMD_COUNT)
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_ARG),a
	ld	a,GFX_CMD_COLOR(ix)
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_CLR),a
	ld	hl,#gfx_v9958_block+14
	ld	a,GFX_CMD_FLAGS(ix)	; the operation nibble
	or	#GFX_V9958_CMD_LINE
	ld	(hl),a
	jp	gfx_v9958_issue_common

; ---------------------------------------------------------------------------
; void gfx_v9958_io_run_pset(const struct gfx_cmd *record);  HL=record
;   PSET: DX/DY and CLR; the code carries the operation nibble.
; ---------------------------------------------------------------------------
_gfx_v9958_io_run_pset::
	push	ix
	push	hl
	pop	ix			; IX = the record
	ld	hl,#0
	ld	(#gfx_v9958_block+0),hl
	ld	(#gfx_v9958_block+2),hl
	ld	l,GFX_CMD_DX(ix)
	ld	h,GFX_CMD_DX+1(ix)
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_DX),hl
	ld	l,GFX_CMD_DY(ix)
	ld	h,GFX_CMD_DY+1(ix)
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_DY),hl
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_NX),hl
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_NY),hl
	ld	a,#0x00
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_ARG),a
	ld	a,GFX_CMD_COLOR(ix)
	ld	(#gfx_v9958_block+GFX_V9958_BLOCK_CLR),a
	ld	hl,#gfx_v9958_block+14
	ld	a,GFX_CMD_FLAGS(ix)	; the operation nibble
	or	#GFX_V9958_CMD_PSET
	ld	(hl),a
; the shared finish: restore IX and issue the block.
gfx_v9958_issue_common:
	pop	ix
	jp	gfx_v9958_issue

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
; void gfx_v9958_io_text_row(const struct gfx_text_row_args *args);  HL=args
;   One scanline of a text string: for each glyph, the font's row byte
;   and its expansion to the data port, the VRAM address already set by
;   the C side.  `mode` selects the expansion:
;     0 GRAPHIC 6, 6x8: three table lookups (three 256-byte pages);
;     1 GRAPHIC 6, 4x6: one 32-byte pair table (two bytes per row);
;     2 GRAPHIC 7, 6x8: six pixels, one byte each;
;     3 GRAPHIC 7, 4x6: four pixels, one byte each.
;   Clobbers A, BC, DE, HL, IX.
; ---------------------------------------------------------------------------
GFX_TEXT_ARGS_TEXT      = 0
GFX_TEXT_ARGS_FONT      = 2
GFX_TEXT_ARGS_TABLE0    = 4
GFX_TEXT_ARGS_TABLE1    = 6
GFX_TEXT_ARGS_TABLE2    = 8
GFX_TEXT_ARGS_COUNT     = 10
GFX_TEXT_ARGS_MODE      = 11
GFX_TEXT_ARGS_FG        = 12
GFX_TEXT_ARGS_BG        = 13

_gfx_v9958_io_text_row::
	push	ix
	push	hl
	pop	ix			; IX = the row's arguments
	ld	a,(_gfx_v9958_port_data)
	ld	c,a			; C = the data port (out (c),a)
	ld	l,GFX_TEXT_ARGS_TEXT(ix)
	ld	h,GFX_TEXT_ARGS_TEXT+1(ix)
	ex	de,hl			; DE = the string's codes
	ld	a,GFX_TEXT_ARGS_COUNT(ix)
	ld	b,a
	ld	a,GFX_TEXT_ARGS_MODE(ix)
	cp	#1
	jp	c,gfx_v9958_text_g6
	jp	z,gfx_v9958_text_g6_4
	cp	#3
	jp	z,gfx_v9958_text_g7_4
; GRAPHIC 7, 6x8: six pixels per row, the font byte's bits 7..2,
; unrolled (H holds the foreground, L the background; DE walks the
; text).
gfx_v9958_text_g7:
	ld	a,GFX_TEXT_ARGS_FG(ix)
	ld	h,a
	ld	a,GFX_TEXT_ARGS_BG(ix)
	ld	l,a
gfx_v9958_text_g7_loop:
	ld	a,(de)			; the glyph's code
	inc	de
	; The font byte: font[code * 8], the row pre-added in C.
	push	hl			; save H = fg, L = bg
	ld	l,a
	ld	h,#0
	add	hl,hl
	add	hl,hl
	add	hl,hl
	ld	a,GFX_TEXT_ARGS_FONT(ix)
	add	a,l
	ld	l,a
	ld	a,GFX_TEXT_ARGS_FONT+1(ix)
	adc	a,h
	ld	h,a
	ld	a,(hl)			; the font row byte
	pop	hl			; H = fg, L = bg again
	; pixel 0: bit 7
	rla
	push	af
	jr	c,gfx_v9958_text_g7_p0
	ld	a,l
	jr	gfx_v9958_text_g7_p0_out
gfx_v9958_text_g7_p0:
	ld	a,h
gfx_v9958_text_g7_p0_out:
	out	(c),a
	pop	af
	rla
	push	af
	jr	c,gfx_v9958_text_g7_p1
	ld	a,l
	jr	gfx_v9958_text_g7_p1_out
gfx_v9958_text_g7_p1:
	ld	a,h
gfx_v9958_text_g7_p1_out:
	out	(c),a
	pop	af
	rla
	push	af
	jr	c,gfx_v9958_text_g7_p2
	ld	a,l
	jr	gfx_v9958_text_g7_p2_out
gfx_v9958_text_g7_p2:
	ld	a,h
gfx_v9958_text_g7_p2_out:
	out	(c),a
	pop	af
	rla
	push	af
	jr	c,gfx_v9958_text_g7_p3
	ld	a,l
	jr	gfx_v9958_text_g7_p3_out
gfx_v9958_text_g7_p3:
	ld	a,h
gfx_v9958_text_g7_p3_out:
	out	(c),a
	pop	af
	rla
	push	af
	jr	c,gfx_v9958_text_g7_p4
	ld	a,l
	jr	gfx_v9958_text_g7_p4_out
gfx_v9958_text_g7_p4:
	ld	a,h
gfx_v9958_text_g7_p4_out:
	out	(c),a
	pop	af
	rla
	push	af
	jr	c,gfx_v9958_text_g7_p5
	ld	a,l
	jr	gfx_v9958_text_g7_p5_out
gfx_v9958_text_g7_p5:
	ld	a,h
gfx_v9958_text_g7_p5_out:
	out	(c),a
	pop	af
	djnz	gfx_v9958_text_g7_loop
	pop	ix
	ret

; GRAPHIC 6, 6x8: the pair's three pages map the font byte.
gfx_v9958_text_g6:
gfx_v9958_text_g6_loop:
	ld	a,(de)
	inc	de
	ld	l,a
	ld	h,#0
	add	hl,hl
	add	hl,hl
	add	hl,hl
	ld	a,GFX_TEXT_ARGS_FONT(ix)
	add	a,l
	ld	l,a
	ld	a,GFX_TEXT_ARGS_FONT+1(ix)
	adc	a,h
	ld	h,a
	ld	a,(hl)			; the font row byte
	push	af
	ld	l,a
	ld	h,#0
	ld	a,GFX_TEXT_ARGS_TABLE0(ix)
	add	a,l
	ld	l,a
	ld	a,GFX_TEXT_ARGS_TABLE0+1(ix)
	adc	a,h
	ld	h,a
	ld	a,(hl)
	out	(c),a
	pop	af
	push	af
	ld	l,a
	ld	h,#0
	ld	a,GFX_TEXT_ARGS_TABLE1(ix)
	add	a,l
	ld	l,a
	ld	a,GFX_TEXT_ARGS_TABLE1+1(ix)
	adc	a,h
	ld	h,a
	ld	a,(hl)
	out	(c),a
	pop	af
	ld	l,a
	ld	h,#0
	ld	a,GFX_TEXT_ARGS_TABLE2(ix)
	add	a,l
	ld	l,a
	ld	a,GFX_TEXT_ARGS_TABLE2+1(ix)
	adc	a,h
	ld	h,a
	ld	a,(hl)
	out	(c),a
	djnz	gfx_v9958_text_g6_loop
	pop	ix
	ret

; GRAPHIC 6, 4x6: the pair table maps the font byte's high nibble to
; the cell's two bytes.
gfx_v9958_text_g6_4:
gfx_v9958_text_g6_4_loop:
	ld	a,(de)
	inc	de
	ld	l,a
	ld	h,#0
	add	hl,hl
	add	hl,hl
	add	hl,hl			; code * 8, the font's stride
	ld	a,GFX_TEXT_ARGS_FONT(ix)
	add	a,l
	ld	l,a
	ld	a,GFX_TEXT_ARGS_FONT+1(ix)
	adc	a,h
	ld	h,a
	ld	a,(hl)			; the font row byte
	and	#0xF0			; the four pixels
	rrca				; the pair table's index,
	rrca				; bits 7..4 to bits 4..1
	rrca
	and	#0x1E			; index * 2
	ld	l,a
	ld	h,#0
	ld	a,GFX_TEXT_ARGS_TABLE0(ix)
	add	a,l
	ld	l,a
	ld	a,GFX_TEXT_ARGS_TABLE0+1(ix)
	adc	a,h
	ld	h,a
	ld	a,(hl)
	out	(c),a
	inc	hl
	ld	a,(hl)
	out	(c),a
	djnz	gfx_v9958_text_g6_4_loop
	pop	ix
	ret

; GRAPHIC 7, 4x6: four pixels per row, the font byte's bits 7..4,
; unrolled (H = foreground, L = background).
gfx_v9958_text_g7_4:
	ld	a,GFX_TEXT_ARGS_FG(ix)
	ld	h,a
	ld	a,GFX_TEXT_ARGS_BG(ix)
	ld	l,a
gfx_v9958_text_g7_4_loop:
	ld	a,(de)			; the glyph's code
	inc	de
	push	hl
	ld	l,a
	ld	h,#0
	add	hl,hl
	add	hl,hl
	add	hl,hl
	ld	a,GFX_TEXT_ARGS_FONT(ix)
	add	a,l
	ld	l,a
	ld	a,GFX_TEXT_ARGS_FONT+1(ix)
	adc	a,h
	ld	h,a
	ld	a,(hl)
	pop	hl
	rla
	push	af
	jr	c,gfx_v9958_text_g7_4_p0
	ld	a,l
	jr	gfx_v9958_text_g7_4_p0_out
gfx_v9958_text_g7_4_p0:
	ld	a,h
gfx_v9958_text_g7_4_p0_out:
	out	(c),a
	pop	af
	rla
	push	af
	jr	c,gfx_v9958_text_g7_4_p1
	ld	a,l
	jr	gfx_v9958_text_g7_4_p1_out
gfx_v9958_text_g7_4_p1:
	ld	a,h
gfx_v9958_text_g7_4_p1_out:
	out	(c),a
	pop	af
	rla
	push	af
	jr	c,gfx_v9958_text_g7_4_p2
	ld	a,l
	jr	gfx_v9958_text_g7_4_p2_out
gfx_v9958_text_g7_4_p2:
	ld	a,h
gfx_v9958_text_g7_4_p2_out:
	out	(c),a
	pop	af
	rla
	push	af
	jr	c,gfx_v9958_text_g7_4_p3
	ld	a,l
	jr	gfx_v9958_text_g7_4_p3_out
gfx_v9958_text_g7_4_p3:
	ld	a,h
gfx_v9958_text_g7_4_p3_out:
	out	(c),a
	pop	af
	djnz	gfx_v9958_text_g7_4_loop
	pop	ix
	ret
