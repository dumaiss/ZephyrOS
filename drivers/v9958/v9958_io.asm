; -----------------------------------------------------------------------------
; V9958 video port I/O and streaming primitives (drivers/v9958, ZMOD module).
;
; Every A0h-A4h access for the VIDEO class lives here; v9958.c owns the
; sequencing, register values, palette table, atlas conversion and the
; class operation validation.  The timing-critical pieces are in this
; file so the proven behavior does not depend on compiler output:
;
;   - the bounded bootstrap delay (software pacing while R#25.WTE=0);
;   - the paced register write (value, delay, register|80h, delay);
;   - the native-WAIT register write (value, register|80h);
;   - the status-register read and the CE completion wait;
;   - the four port streams (control/data/palette/indirect) used for
;     operations, VRAM addressing bytes, atlas rows and command blocks.
;
; Call convention (sdcccall(0), the default of this project's SDCC):
;   void   f(uint8_t value)              A = value
;   void   f(uint8_t value, uint8_t reg) A = value, L = reg
;   void   f(struct v9958_block *b)      HL = block
;   uint8_t f(void)                      A = L = result
;
; struct v9958_block layout (driver-private):
;   src    0..1  const uint8_t *source
;   length 2..3  uint16_t byte count (1..65535)
; The streams deliberately accept any 16-bit length: the historical
; 16-byte non-block ceiling belongs to the old Virtual Drip transport
; contract, not to this directly attached chip.
; -----------------------------------------------------------------------------

	.module v9958_io
	.include "zephyr80/hardware.inc"

	.globl _v9958_io_bootstrap_delay
	.globl _v9958_io_reg_paced
	.globl _v9958_io_reg
	.globl _v9958_io_status_read
	.globl _v9958_io_wait_command
	.globl _v9958_io_latch_write
	.globl _v9958_io_stream_ctrl
	.globl _v9958_io_stream_data
	.globl _v9958_io_stream_palette
	.globl _v9958_io_stream_indirect

	.area _CODE

; ---------------------------------------------------------------------------
; void v9958_io_bootstrap_delay(void);
;   Bounded software pacing for accesses made while R#25.WTE=0, exactly
;   the proven count: the loop is register-only, preserves every caller
;   register, and never relies on incidental instruction timing.
; ---------------------------------------------------------------------------
_v9958_io_bootstrap_delay::
	push	bc
	ld	b,#ZOS_V9958_BOOT_DELAY_COUNT
v9958_delay_loop:
	djnz	v9958_delay_loop
	pop	bc
	ret

; ---------------------------------------------------------------------------
; void v9958_io_reg_paced(uint8_t value, uint8_t reg);  A=value, L=reg
;   Bootstrap register write: value, paced, register | 80h, paced.  Only
;   valid while the porch is bypassed and WTE=0.
; ---------------------------------------------------------------------------
_v9958_io_reg_paced::
	out	(ZOS_V9958_CMD_PORT),a
	call	_v9958_io_bootstrap_delay
	ld	a,l
	or	#ZOS_V9958_REG_SELECT
	out	(ZOS_V9958_CMD_PORT),a
	jp	_v9958_io_bootstrap_delay

; ---------------------------------------------------------------------------
; void v9958_io_reg(uint8_t value, uint8_t reg);  A=value, L=reg
;   Native-WAIT register write: value, register | 80h.  Requires WTE=1
;   and the U11 porch enabled; the hardware holds the I/O cycle.
; ---------------------------------------------------------------------------
_v9958_io_reg::
	out	(ZOS_V9958_CMD_PORT),a
	ld	a,l
	or	#ZOS_V9958_REG_SELECT
	out	(ZOS_V9958_CMD_PORT),a
	ret

; ---------------------------------------------------------------------------
; uint8_t v9958_io_status_read(void);  A = L = selected status register
;   R#15 stays at 2 (command-engine status) while the driver owns the
;   VDP: bit 0 is CE, "a command is active".
; ---------------------------------------------------------------------------
_v9958_io_status_read::
	in	a,(ZOS_V9958_CMD_PORT)
	ld	l,a
	ret

; ---------------------------------------------------------------------------
; void v9958_io_wait_command(void);
;   Block until S#2.CE clears.  Never called from an ISR; a command that
;   never completes is a hardware fault the driver cannot mask.
; ---------------------------------------------------------------------------
_v9958_io_wait_command::
v9958_wait_cmd_loop:
	in	a,(ZOS_V9958_CMD_PORT)
	and	#ZOS_V9958_STATUS2_CE
	jr	nz,v9958_wait_cmd_loop
	ret

; ---------------------------------------------------------------------------
; void v9958_io_latch_write(uint8_t value);  A=value
;   LunchCrema configuration latch.  The C caller owns the shadow and
;   preserves D0 (interrupt route) on every write; this primitive only
;   emits the byte.
; ---------------------------------------------------------------------------
_v9958_io_latch_write::
	out	(ZOS_V9958_CONFIG_PORT),a
	ret

; ---------------------------------------------------------------------------
; Streaming primitives.  Each one loads {src, length} from the block in
; HL, then writes every byte to its port.  A zero length is accepted by
; the primitive (no bytes); the C class dispatcher validates non-zero
; length before any I/O.
; ---------------------------------------------------------------------------

; Load the block in HL; return HL=src, BC=length, Z set when length==0.
v9958_load_block:
	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	inc	hl
	ld	c,(hl)
	inc	hl
	ld	b,(hl)
	ex	de,hl
	ld	a,b
	or	c
	ret

; void v9958_io_stream_ctrl(struct v9958_block *block);
_v9958_io_stream_ctrl::
	call	v9958_load_block
	ret	z
v9958_ctrl_loop:
	ld	a,(hl)
	out	(ZOS_V9958_CMD_PORT),a
	inc	hl
	dec	bc
	ld	a,b
	or	c
	jr	nz,v9958_ctrl_loop
	ret

; void v9958_io_stream_data(struct v9958_block *block);
_v9958_io_stream_data::
	call	v9958_load_block
	ret	z
v9958_data_loop:
	ld	a,(hl)
	out	(ZOS_V9958_DATA_PORT),a
	inc	hl
	dec	bc
	ld	a,b
	or	c
	jr	nz,v9958_data_loop
	ret

; void v9958_io_stream_palette(struct v9958_block *block);
_v9958_io_stream_palette::
	call	v9958_load_block
	ret	z
v9958_palette_loop:
	ld	a,(hl)
	out	(ZOS_V9958_PALETTE_PORT),a
	inc	hl
	dec	bc
	ld	a,b
	or	c
	jr	nz,v9958_palette_loop
	ret

; void v9958_io_stream_indirect(struct v9958_block *block);
_v9958_io_stream_indirect::
	call	v9958_load_block
	ret	z
v9958_indirect_loop:
	ld	a,(hl)
	out	(ZOS_V9958_INDIRECT_PORT),a
	inc	hl
	dec	bc
	ld	a,b
	or	c
	jr	nz,v9958_indirect_loop
	ret
