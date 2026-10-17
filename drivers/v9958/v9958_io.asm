; -----------------------------------------------------------------------------
; V9958 video port I/O and streaming primitives (drivers/v9958, MOD module).
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
;     operations, VRAM addressing bytes, atlas rows and command blocks;
;   - the renderer's hot path: the print run's glyph copies and the
;     printable-byte fast path in front of the C parser.
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
	.include "hardware.inc"

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
	.globl _v9958_char_putc
	.globl _v9958_char_write
	.globl _v9958_char_putc_worker
	.globl _v9958_char_write_worker
	.globl _v9958_out_flush
	.globl _v9958_term_write
	.globl _v9958_term_feed
	.globl _v9958_state
	.globl _v9958_run_ptr

; struct v9958_state offsets and the renderer constants (pinned by the
; _Static_asserts in v9958_driver.h).
V9958_ST_ORIGIN    = 1
V9958_ST_TERM      = 2
V9958_ST_ESC       = 3
V9958_ST_COL       = 9
V9958_ST_ROW       = 10
V9958_ST_ATTR      = 12
V9958_ST_RUN_COUNT = 17
V9958_ST_RUN_COL   = 18
V9958_ST_RUN_ROW   = 19
V9958_RUN_SIZE     = 64
V9958_LAST_COLUMN  = 84
V9958_CELL_WIDTH   = 6
V9958_CMD_HMMM     = 0xD0

	.area _CODE

; ---------------------------------------------------------------------------
; void v9958_io_bootstrap_delay(void);
;   Bounded software pacing for accesses made while R#25.WTE=0, exactly
;   the proven count: the loop is register-only, preserves every caller
;   register, and never relies on incidental instruction timing.
; ---------------------------------------------------------------------------
_v9958_io_bootstrap_delay::
	push	bc
	ld	b,#KOS_V9958_BOOT_DELAY_COUNT
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
	out	(KOS_V9958_CMD_PORT),a
	call	_v9958_io_bootstrap_delay
	ld	a,l
	or	#KOS_V9958_REG_SELECT
	out	(KOS_V9958_CMD_PORT),a
	jp	_v9958_io_bootstrap_delay

; ---------------------------------------------------------------------------
; void v9958_io_reg(uint8_t value, uint8_t reg);  A=value, L=reg
;   Native-WAIT register write: value, register | 80h.  Requires WTE=1
;   and the U11 porch enabled; the hardware holds the I/O cycle.
; ---------------------------------------------------------------------------
_v9958_io_reg::
	out	(KOS_V9958_CMD_PORT),a
	ld	a,l
	or	#KOS_V9958_REG_SELECT
	out	(KOS_V9958_CMD_PORT),a
	ret

; ---------------------------------------------------------------------------
; uint8_t v9958_io_status_read(void);  A = L = selected status register
;   R#15 stays at 2 (command-engine status) while the driver owns the
;   VDP: bit 0 is CE, "a command is active".
; ---------------------------------------------------------------------------
_v9958_io_status_read::
	in	a,(KOS_V9958_CMD_PORT)
	ld	l,a
	ret

; ---------------------------------------------------------------------------
; void v9958_io_wait_command(void);
;   Block until S#2.CE clears.  Never called from an ISR; a command that
;   never completes is a hardware fault the driver cannot mask.
; ---------------------------------------------------------------------------
_v9958_io_wait_command::
v9958_wait_cmd_loop:
	in	a,(KOS_V9958_CMD_PORT)
	and	#KOS_V9958_STATUS2_CE
	jr	nz,v9958_wait_cmd_loop
	ret

; ---------------------------------------------------------------------------
; void v9958_io_latch_write(uint8_t value);  A=value
;   LunchCrema configuration latch.  The C caller owns the shadow and
;   preserves D0 (interrupt route) on every write; this primitive only
;   emits the byte.
; ---------------------------------------------------------------------------
_v9958_io_latch_write::
	out	(KOS_V9958_CONFIG_PORT),a
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
	out	(KOS_V9958_CMD_PORT),a
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
	out	(KOS_V9958_DATA_PORT),a
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
	out	(KOS_V9958_PALETTE_PORT),a
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
	out	(KOS_V9958_INDIRECT_PORT),a
	inc	hl
	dec	bc
	ld	a,b
	or	c
	jr	nz,v9958_indirect_loop
	ret

; ===========================================================================
; Renderer hot path
; ===========================================================================

; The glyph copy's command block, R#32..R#46: SX, SY, DX, DY, NX, NY,
; colour, argument, command.  The fixed fields come from the template
; once per run; each glyph patches SX, SY and DX before it waits for the
; previous command, so building the next block overlaps the engine.
	.area _DRIVER_IO
v9958_glyph_cmd:
	.ds	15

	.area _CODE
v9958_glyph_template:
	.db	0, 0		; SX: per glyph (the low byte only, at most 186)
	.db	0, 1		; SY: per glyph; the atlas lives in page one
	.db	0, 0		; DX: per glyph
	.db	0, 0		; DY: the run's row
	.db	V9958_CELL_WIDTH, 0
	.db	8, 0		; NY: one cell
	.db	0, 0		; colour, argument
	.db	V9958_CMD_HMMM

; ---------------------------------------------------------------------------
; void v9958_out_flush(void);
;   Render the pending print run at (out_run_col, out_run_row) and empty
;   it: per character one HMMM 6x8 copy from cell (ch & 1Fh, ch >> 5) of
;   the normal atlas, or of the reverse one (40h lines lower) while SGR
;   reverse is set; destination row y = row * 8 + the scroll origin.
; ---------------------------------------------------------------------------
_v9958_out_flush::
	ld	a,(_v9958_state+V9958_ST_RUN_COUNT)
	or	a
	ret	z
	ld	hl,#v9958_glyph_template
	ld	de,#v9958_glyph_cmd
	ld	bc,#KOS_V9958_CMD_REGISTER_COUNT
	ldir
	; DY = row * 8 + origin
	ld	a,(_v9958_state+V9958_ST_RUN_ROW)
	add	a,a
	add	a,a
	add	a,a
	ld	hl,#_v9958_state+V9958_ST_ORIGIN
	add	a,(hl)
	ld	(v9958_glyph_cmd+6),a
	; DX = column * 6
	ld	a,(_v9958_state+V9958_ST_RUN_COL)
	ld	l,a
	ld	h,#0
	add	hl,hl
	ld	d,h
	ld	e,l
	add	hl,hl
	add	hl,de
	ld	(v9958_glyph_cmd+4),hl
	; C = the atlas line offset: 40h for reverse video
	ld	a,(_v9958_state+V9958_ST_ATTR)
	rrca
	and	#0x80
	rrca
	ld	c,a
	ld	a,(_v9958_state+V9958_ST_RUN_COUNT)
	ld	b,a
	ld	de,(_v9958_run_ptr)
v9958_glyph_loop:
	ld	a,(de)
	inc	de
	ld	h,a
	and	#0x1F
	ld	l,a
	add	a,a
	add	a,l
	add	a,a
	ld	(v9958_glyph_cmd+0),a	; SX = (ch & 1Fh) * 6
	ld	a,h
	and	#0xE0
	rrca
	rrca
	or	c
	ld	(v9958_glyph_cmd+2),a	; SY = (ch >> 5) * 8 | reverse
v9958_glyph_wait:
	in	a,(KOS_V9958_CMD_PORT)
	and	#KOS_V9958_STATUS2_CE
	jr	nz,v9958_glyph_wait
	ld	a,#KOS_V9958_CMD_REGISTER_BASE
	out	(KOS_V9958_CMD_PORT),a
	ld	a,#(KOS_V9958_REG_R17 | KOS_V9958_REG_SELECT)
	out	(KOS_V9958_CMD_PORT),a
	push	bc
	ld	hl,#v9958_glyph_cmd
	ld	bc,#(KOS_V9958_CMD_REGISTER_COUNT * 256 + KOS_V9958_INDIRECT_PORT)
	otir				; R#46 last: starts the copy
	ld	hl,(v9958_glyph_cmd+4)
	ld	bc,#V9958_CELL_WIDTH
	add	hl,bc
	ld	(v9958_glyph_cmd+4),hl
	pop	bc
	djnz	v9958_glyph_loop
	xor	a
	ld	(_v9958_state+V9958_ST_RUN_COUNT),a
	ret

; ---------------------------------------------------------------------------
; void v9958_term_write(const uint8_t *buf, uint16_t len);  HL=buf, DE=len
;   The write path's byte loop.  A printable byte (20h and up) while the
;   parser is in its normal state, with room in the run and the cursor
;   left of the last column, is appended here exactly as
;   v9958_put_printable appends it (the triple-ESC count cleared, the
;   run's origin taken by its first byte, the cursor one cell right).
;   Every other byte goes to v9958_term_feed (A = byte).
; ---------------------------------------------------------------------------
_v9958_term_write::
	ld	a,d
	or	e
	ret	z
v9958_tw_loop:
	ld	a,(_v9958_state+V9958_ST_TERM)
	or	a
	jr	nz,v9958_tw_slow
	ld	a,(hl)
	cp	#0x20
	jr	c,v9958_tw_slow
	ld	a,(_v9958_state+V9958_ST_RUN_COUNT)
	cp	#V9958_RUN_SIZE
	jr	z,v9958_tw_slow
	ld	c,a			; C = run count
	ld	a,(_v9958_state+V9958_ST_COL)
	cp	#V9958_LAST_COLUMN
	jr	nc,v9958_tw_slow
	ld	b,a			; B = column
	xor	a
	ld	(_v9958_state+V9958_ST_ESC),a
	or	c
	jr	nz,v9958_tw_append
	ld	a,b
	ld	(_v9958_state+V9958_ST_RUN_COL),a
	ld	a,(_v9958_state+V9958_ST_ROW)
	ld	(_v9958_state+V9958_ST_RUN_ROW),a
v9958_tw_append:
	push	de
	push	bc
	ld	a,(hl)
	ex	de,hl			; DE = buf
	ld	hl,(_v9958_run_ptr)
	ld	b,#0
	add	hl,bc
	ld	(hl),a			; run[count] = byte
	ex	de,hl			; HL = buf
	pop	bc
	pop	de
	inc	c
	ld	a,c
	ld	(_v9958_state+V9958_ST_RUN_COUNT),a
	inc	b
	ld	a,b
	ld	(_v9958_state+V9958_ST_COL),a
	jr	v9958_tw_next
v9958_tw_slow:
	ld	a,(hl)
	push	hl
	push	de
	call	_v9958_term_feed
	pop	de
	pop	hl
v9958_tw_next:
	inc	hl
	dec	de
	ld	a,d
	or	e
	jr	nz,v9958_tw_loop
	ret

; ===========================================================================
; Renderer private stack (module BSS, _DATA)
; ===========================================================================
;
; The kernel dispatches system calls on the small common gate stack:
; kos_gate_stack_top sits only ~70 bytes above the common syscall state,
; and the SIO console path was designed to fit in that budget.  The
; ported text renderer's call chain (parser -> control action -> flush ->
; glyph HMMM -> command start) is deeper than that budget, so both
; The terminal entries run the C worker on a private driver stack instead:
; the caller's SP, return address and arguments are parked in module BSS,
; SP moves to _v9958_alt_stack_top, the worker runs there, and SP plus
; the return path are restored exactly.  The renderer is single-depth by
; construction (never nested, never re-entered), so one save area and one
; stack are sufficient; the helpers are not ISR-safe.
;
; The entries implement the unchanged sdcccall(1) prototypes:
;   int (*putc)(void *state, char ch)               state HL, ch stack
;   int (*write)(void *state, const char *buf, u16) state HL, buf DE,
;                                                   len stack, result DE

	.area _DRIVER_IO
_v9958_alt_saved_sp::
	.ds	2
_v9958_alt_ret::
	.ds	2
_v9958_alt_state::
	.ds	2
_v9958_alt_buf::
	.ds	2
_v9958_alt_word::
	.ds	2
_v9958_alt_result::
	.ds	2
_v9958_alt_stack::
	.ds	256
_v9958_alt_stack_top::

	.area _CODE

_v9958_char_putc::
	pop	bc
	ld	(_v9958_alt_ret),bc
	ld	(_v9958_alt_state),hl
	pop	hl			; HL = {char, pad}: one-byte stack arg
	ld	(_v9958_alt_word),hl
	dec	sp			; back to the caller's stack level
	ld	(_v9958_alt_saved_sp),sp
	ld	sp,#_v9958_alt_stack_top
	ld	hl,(_v9958_alt_state)
	ld	bc,(_v9958_alt_word)
	push	bc			; char argument (low byte)
	call	_v9958_char_putc_worker
	ld	(_v9958_alt_result),de
	ld	sp,(_v9958_alt_saved_sp)
	ld	hl,(_v9958_alt_ret)
	ld	de,(_v9958_alt_result)
	jp	(hl)

_v9958_char_write::
	pop	bc
	ld	(_v9958_alt_ret),bc
	ld	(_v9958_alt_state),hl
	ld	(_v9958_alt_buf),de
	ex	(sp),hl			; HL = length, (sp) = scratch
	ld	(_v9958_alt_word),hl
	pop	hl			; drop the consumed stack word
	ld	(_v9958_alt_saved_sp),sp
	ld	sp,#_v9958_alt_stack_top
	ld	hl,(_v9958_alt_state)
	ld	de,(_v9958_alt_buf)
	ld	bc,(_v9958_alt_word)
	push	bc			; 16-bit length argument
	call	_v9958_char_write_worker
	ld	(_v9958_alt_result),de
	ld	sp,(_v9958_alt_saved_sp)
	ld	hl,(_v9958_alt_ret)
	ld	de,(_v9958_alt_result)
	jp	(hl)
