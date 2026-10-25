; -----------------------------------------------------------------------------
; libgfx: text (docs/gfx-sdk.md sections 12 to 14).
;
; int gfx_text(uint16_t x, uint16_t y, const char *text,
;              const struct gfx_text_style *style);
; int gfx_text_n(uint16_t x, uint16_t y, const char *text, uint16_t length,
;                const struct gfx_text_style *style);
;
; Opaque text is scanline-major: one VRAM address per scanline of the
; string, then every glyph's bytes for that scanline through the data
; port.  The RAM fonts are row-major (gfx_font.c, gfx_font4.c): one
; 256-byte page per glyph row, indexed by the character code, holding the
; row's pixels pre-scaled as an index into the colour pair's expansion
; table (6x8: pixels * 4; 4x6: pixels * 2).  The expansion tables sit at
; the start of 256-byte pages too, built for the current colour pair:
;   kind 0, GRAPHIC 6 6x8: 64 entries of 4 bytes (3 used), page A;
;   kind 1, GRAPHIC 6 4x6: 16 entries of 2 bytes, page A;
;   kind 2, GRAPHIC 7 6x8: pixels 0-3 at index * 4 on page A, pixels 4-5
;           at index * 4 on page B (page A + 1);
;   kind 3, GRAPHIC 7 4x6: 16 entries of 4 bytes, page A.
; The inner loop is then the character, the font byte, the table and the
; OUTs, the alternate registers holding the font page (H'), the table
; page (D') and the data port (C').
;
; Transparent text goes to the glyph atlas (gfx_atlas_draw.asm).
; -----------------------------------------------------------------------------

	.module gfx_text

	.globl _gfx_text
	.globl _gfx_text_n
	.globl gfx_text_emit
	.globl gfx_fit
	.globl gfx_e_x
	.globl gfx_e_y
	.globl gfx_e_ptr
	.globl gfx_e_n
	.globl gfx_e_fg
	.globl gfx_e_bg
	.globl gfx_e_font

	.globl gfx_atlas_draw
	.globl gfx_ad_x
	.globl gfx_ad_y
	.globl gfx_ad_ptr
	.globl gfx_ad_len
	.globl gfx_ad_fg

	.globl gfx_wait_ce
	.globl gfx_ret_zero
	.globl gfx_ret_einval
	.globl gfx_ret_enotsup
	.globl _gfx_flush
	.globl _gfx_draw_ready
	.globl _gfx_draw_mode
	.globl _gfx_draw_width
	.globl _gfx_v9958_port_cmd
	.globl _gfx_v9958_port_data
	.globl _gfx_font6_page
	.globl _gfx_font4_page
	.globl _gfx_pair_page
	.globl _gfx_font4_install

GFX_MODE_G6             = 0
GFX_MODE_YJK            = 2
GFX_TEXT_TRANSPARENT    = 0x01

	.area _DATA

t_x:
	.ds 2
t_y:
	.ds 2
t_ptr:
	.ds 2
t_len:
	.ds 2
t_style:
	.ds 2
t_flags:
	.ds 1

; The emit's arguments (the atlas fills them too).
gfx_e_x::
	.ds 2
gfx_e_y::
	.ds 2
gfx_e_ptr::
	.ds 2
gfx_e_n::
	.ds 1
gfx_e_fg::
	.ds 1
gfx_e_bg::
	.ds 1
gfx_e_font::
	.ds 1

e_kind:
	.ds 1
e_col:
	.ds 1
e_rows:
	.ds 1
e_line:
	.ds 2
e_loop:
	.ds 2

; The expansion tables' key: built for this pair and kind.
pk_valid:
	.ds 1
pk_fg:
	.ds 1
pk_bg:
	.ds 1
pk_kind:
	.ds 1

	.area _CODE

; ---------------------------------------------------------------------------
; The entry points.
; ---------------------------------------------------------------------------
_gfx_text::
	ld	(t_x),hl
	ld	(t_y),de
	pop	bc
	pop	hl
	ld	(t_ptr),hl
	pop	de
	ld	(t_style),de
	push	bc
	ld	a,h			; the length: strlen, or 0 without text
	or	l
	jr	z,gfx_text_len
	ld	d,h
	ld	e,l
	xor	a
	ld	b,a
	ld	c,a
	cpir
	scf
	sbc	hl,de			; past the NUL - start - 1
gfx_text_len:
	ld	(t_len),hl
	jr	gfx_text_common

_gfx_text_n::
	ld	(t_x),hl
	ld	(t_y),de
	pop	bc
	pop	hl
	ld	(t_ptr),hl
	pop	hl
	ld	(t_len),hl
	pop	hl
	ld	(t_style),hl
	push	bc

gfx_text_common:
	ld	a,(_gfx_draw_ready)
	or	a
	jp	z,gfx_ret_einval
	ld	a,(_gfx_draw_mode)
	cp	#GFX_MODE_YJK
	jp	z,gfx_ret_enotsup
	ld	hl,(t_style)
	ld	a,h
	or	l
	jp	z,gfx_ret_einval
	ld	de,(t_ptr)		; no text with a length
	ld	a,d
	or	e
	jr	nz,gfx_text_style
	ld	de,(t_len)
	ld	a,d
	or	e
	jp	nz,gfx_ret_einval
gfx_text_style:
	ld	a,(hl)			; HL = the style: fg, bg, flags, font
	ld	(gfx_e_fg),a
	inc	hl
	ld	a,(hl)
	ld	(gfx_e_bg),a
	inc	hl
	ld	a,(hl)
	ld	(t_flags),a
	inc	hl
	ld	a,(hl)
	cp	#2
	jp	nc,gfx_ret_einval
	ld	(gfx_e_font),a
	ld	a,(t_flags)
	and	#~GFX_TEXT_TRANSPARENT
	jp	nz,gfx_ret_einval
	ld	a,(_gfx_draw_mode)
	or	a
	jr	nz,gfx_text_colours_ok
	ld	a,(gfx_e_fg)
	ld	c,a
	ld	a,(gfx_e_bg)
	or	c
	and	#0xF0
	jp	nz,gfx_ret_einval
gfx_text_colours_ok:
	ld	a,(t_flags)
	and	#GFX_TEXT_TRANSPARENT
	jr	z,gfx_text_opaque
	ld	hl,(t_x)
	ld	(gfx_ad_x),hl
	ld	hl,(t_y)
	ld	(gfx_ad_y),hl
	ld	hl,(t_ptr)
	ld	(gfx_ad_ptr),hl
	ld	hl,(t_len)
	ld	(gfx_ad_len),hl
	ld	a,(gfx_e_fg)
	ld	(gfx_ad_fg),a
	jp	gfx_atlas_draw

gfx_text_opaque:
	; GRAPHIC 6 streams whole bytes: x is even
	ld	a,(_gfx_draw_mode)
	or	a
	jr	nz,gfx_text_x_ok
	ld	a,(t_x)
	rrca
	jp	c,gfx_ret_einval
gfx_text_x_ok:
	; y <= 512 - height
	ld	a,(gfx_e_font)
	or	a
	ld	de,#512 - 8
	jr	z,gfx_text_y
	ld	de,#512 - 6
gfx_text_y:
	ld	hl,(t_y)
	ex	de,hl
	or	a
	sbc	hl,de
	jp	c,gfx_ret_einval
	; x < width
	ld	hl,(_gfx_draw_width)
	ld	de,(t_x)
	or	a
	sbc	hl,de			; HL = the room
	jp	c,gfx_ret_einval
	jp	z,gfx_ret_einval
	push	hl
	call	_gfx_flush
	pop	hl
	ld	a,(gfx_e_font)
	or	a
	ld	c,#6
	jr	z,gfx_text_fit
	ld	c,#4
gfx_text_fit:
	ld	de,(t_len)
	call	gfx_fit
	ld	(gfx_e_n),a
	ld	hl,(t_x)
	ld	(gfx_e_x),hl
	ld	hl,(t_y)
	ld	(gfx_e_y),hl
	ld	hl,(t_ptr)
	ld	(gfx_e_ptr),hl
	call	gfx_text_emit
	jp	gfx_ret_zero

; ---------------------------------------------------------------------------
; gfx_fit: HL = the room in dots (1..512), C = the glyph width (4 or 6),
; DE = the length.  A = min(length, room / width), at most 128.
; ---------------------------------------------------------------------------
gfx_fit::
	ld	a,c
	cp	#6
	jr	z,gfx_fit_6
	srl	h
	rr	l
	srl	h
	rr	l			; room / 4
	jr	gfx_fit_min
gfx_fit_6:
	srl	h
	rr	l			; m = room / 2 (<= 256)
	; m / 3 = (m * 171) >> 9, exact for m <= 256 (171 = 128+32+8+2+1)
	push	de
	ld	d,h
	ld	e,l
	add	hl,hl			; 2m
	push	hl
	add	hl,hl
	add	hl,hl			; 8m
	push	hl
	add	hl,hl
	add	hl,hl			; 32m
	push	hl
	add	hl,hl
	add	hl,hl			; 128m
	add	hl,de			; 128m + m
	pop	de
	add	hl,de			; + 32m
	pop	de
	add	hl,de			; + 8m
	pop	de
	add	hl,de			; + 2m
	pop	de
	ld	l,h
	ld	h,#0
	srl	l			; >> 9
gfx_fit_min:
	; HL = the quotient (<= 128); A = min(DE, HL)
	ld	a,d
	or	a
	ld	a,l
	ret	nz			; length >= 256: the quotient
	ld	a,e
	cp	l
	ret	c			; length < quotient
	ld	a,l
	ret

; ---------------------------------------------------------------------------
; gfx_text_emit: the opaque glyphs gfx_e_ptr[0 .. gfx_e_n - 1] at
; (gfx_e_x, gfx_e_y) in gfx_e_fg on gfx_e_bg, font gfx_e_font.  The caller
; validated the geometry; the emit waits for the command engine, so the
; bytes land after every running command.  Clobbers everything but IX.
; ---------------------------------------------------------------------------
gfx_text_emit::
	ld	a,(gfx_e_n)
	or	a
	ret	z
	ld	a,(gfx_e_font)
	or	a
	call	nz,_gfx_font4_install	; the 4x6 RAM copy, made at first use
	call	gfx_wait_ce
	ld	a,(_gfx_draw_mode)
	or	a
	jr	z,gfx_emit_kind
	ld	a,#2
gfx_emit_kind:
	ld	hl,#gfx_e_font
	add	a,(hl)
	ld	(e_kind),a
	call	gfx_text_pairs
	; the column byte: x / 2 in GRAPHIC 6
	ld	hl,(gfx_e_x)
	ld	a,(_gfx_draw_mode)
	or	a
	jr	nz,gfx_emit_col
	srl	h
	rr	l
gfx_emit_col:
	ld	a,l
	ld	(e_col),a
	ld	hl,(gfx_e_y)
	ld	(e_line),hl
	; the inner loop of the kind
	ld	a,(e_kind)
	add	a,a
	ld	l,a
	ld	h,#0
	ld	de,#gfx_emit_loops
	add	hl,de
	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	ld	(e_loop),de
	; the rows and the font's first row page
	ld	a,(gfx_e_font)
	or	a
	ld	a,(_gfx_font6_page)
	ld	b,#8
	jr	z,gfx_emit_font
	ld	a,(_gfx_font4_page)
	ld	b,#6
gfx_emit_font:
	ld	c,a
	ld	a,b
	ld	(e_rows),a
	ld	a,c
	exx
	ld	h,a			; H' = the font row page
	ld	a,(_gfx_pair_page)
	ld	d,a			; D' = the expansion page
	ld	a,(_gfx_v9958_port_data)
	ld	c,a			; C' = the data port
	exx
gfx_emit_row:
	; the scanline's VRAM address: R#14 = line >> 6, then A7-A0 (the
	; column) and A13-A8 (the line's low six bits) with the write bit
	ld	a,(_gfx_v9958_port_cmd)
	ld	c,a
	ld	hl,(e_line)
	ld	a,l
	rlca
	rlca
	and	#3
	bit	0,h
	jr	z,gfx_emit_r14
	or	#4
gfx_emit_r14:
	out	(c),a
	ld	a,#0x8E
	out	(c),a
	ld	a,(e_col)
	out	(c),a
	ld	a,l
	and	#0x3F
	or	#0x40
	out	(c),a
	; the glyphs
	ld	de,(gfx_e_ptr)
	ld	a,(gfx_e_n)
	ld	b,a
	ld	hl,(e_loop)
	call	gfx_jp_hl
	exx
	inc	h			; the next row page
	exx
	ld	hl,(e_line)
	inc	hl
	ld	(e_line),hl
	ld	hl,#e_rows
	dec	(hl)
	jr	nz,gfx_emit_row
	ret

gfx_jp_hl:
	jp	(hl)

gfx_emit_loops:
	.dw	gfx_emit_k0
	.dw	gfx_emit_k1
	.dw	gfx_emit_k2
	.dw	gfx_emit_k3

; kind 0, GRAPHIC 6 6x8: three bytes per glyph row.
gfx_emit_k0:
	ld	a,(de)
	inc	de
	exx
	ld	l,a
	ld	e,(hl)
	ld	a,(de)
	out	(c),a
	inc	e
	ld	a,(de)
	out	(c),a
	inc	e
	ld	a,(de)
	out	(c),a
	exx
	djnz	gfx_emit_k0
	ret

; kind 1, GRAPHIC 6 4x6: two bytes per glyph row.
gfx_emit_k1:
	ld	a,(de)
	inc	de
	exx
	ld	l,a
	ld	e,(hl)
	ld	a,(de)
	out	(c),a
	inc	e
	ld	a,(de)
	out	(c),a
	exx
	djnz	gfx_emit_k1
	ret

; kind 2, GRAPHIC 7 6x8: six bytes per glyph row, four from page A and
; two from page B.
gfx_emit_k2:
	ld	a,(de)
	inc	de
	exx
	ld	l,a
	ld	e,(hl)
	ld	a,(de)
	out	(c),a
	inc	e
	ld	a,(de)
	out	(c),a
	inc	e
	ld	a,(de)
	out	(c),a
	inc	e
	ld	a,(de)
	out	(c),a
	inc	d
	ld	e,(hl)
	ld	a,(de)
	out	(c),a
	inc	e
	ld	a,(de)
	out	(c),a
	dec	d
	exx
	djnz	gfx_emit_k2
	ret

; kind 3, GRAPHIC 7 4x6: four bytes per glyph row (the font byte is the
; pixels * 2; the table's entries are four bytes).
gfx_emit_k3:
	ld	a,(de)
	inc	de
	exx
	ld	l,a
	ld	a,(hl)
	add	a,a
	ld	e,a
	ld	a,(de)
	out	(c),a
	inc	e
	ld	a,(de)
	out	(c),a
	inc	e
	ld	a,(de)
	out	(c),a
	inc	e
	ld	a,(de)
	out	(c),a
	exx
	djnz	gfx_emit_k3
	ret

; ---------------------------------------------------------------------------
; gfx_text_pairs: the expansion tables for (gfx_e_fg, gfx_e_bg, e_kind),
; rebuilt only when the key changes.
; ---------------------------------------------------------------------------
gfx_text_pairs:
	ld	a,(pk_valid)
	or	a
	jr	z,gfx_pairs_build
	ld	a,(e_kind)
	ld	hl,#pk_kind
	cp	(hl)
	jr	nz,gfx_pairs_build
	ld	a,(gfx_e_fg)
	ld	hl,#pk_fg
	cp	(hl)
	jr	nz,gfx_pairs_build
	ld	a,(gfx_e_bg)
	ld	hl,#pk_bg
	cp	(hl)
	ret	z
gfx_pairs_build:
	ld	a,#1
	ld	(pk_valid),a
	ld	a,(e_kind)
	ld	(pk_kind),a
	ld	a,(gfx_e_fg)
	ld	(pk_fg),a
	ld	a,(gfx_e_bg)
	ld	(pk_bg),a
	ld	a,(e_kind)
	cp	#2
	jr	nc,gfx_pairs_g7_kinds
	; GRAPHIC 6: the four two-pixel bytes (00 bg|bg, 01 bg|fg, 10 fg|bg,
	; 11 fg|fg) at page B's start, so a pattern indexes them with L
	ld	a,(_gfx_pair_page)
	ld	h,a
	inc	h
	ld	l,#0
	ld	a,(gfx_e_bg)
	ld	e,a
	ld	a,(gfx_e_fg)
	ld	d,a
	ld	a,e
	add	a,a
	add	a,a
	add	a,a
	add	a,a
	ld	c,a			; bg << 4
	or	e
	ld	(hl),a
	inc	l
	ld	a,c
	or	d
	ld	(hl),a
	inc	l
	ld	a,d
	add	a,a
	add	a,a
	add	a,a
	add	a,a
	ld	c,a			; fg << 4
	or	e
	ld	(hl),a
	inc	l
	ld	a,c
	or	d
	ld	(hl),a
	; DE = page A, the table; H = page B, L = a pattern
	ld	a,(_gfx_pair_page)
	ld	d,a
	ld	e,#0
	ld	a,(e_kind)
	or	a
	jr	z,gfx_pairs_k0
	; kind 1: 16 entries of two bytes, [p(i >> 2), p(i & 3)]
	ld	c,#0
gfx_pairs_k1_loop:
	ld	a,c
	rrca
	rrca
	and	#3
	ld	l,a
	ld	a,(hl)
	ld	(de),a
	inc	e
	ld	a,c
	and	#3
	ld	l,a
	ld	a,(hl)
	ld	(de),a
	inc	e
	inc	c
	ld	a,c
	cp	#16
	jr	nz,gfx_pairs_k1_loop
	ret
gfx_pairs_k0:
	; kind 0: 64 entries of four bytes, [p(i >> 4), p(i >> 2), p(i), -]
	ld	c,#0
gfx_pairs_k0_loop:
	ld	a,c
	rrca
	rrca
	rrca
	rrca
	and	#3
	ld	l,a
	ld	a,(hl)
	ld	(de),a
	inc	e
	ld	a,c
	rrca
	rrca
	and	#3
	ld	l,a
	ld	a,(hl)
	ld	(de),a
	inc	e
	ld	a,c
	and	#3
	ld	l,a
	ld	a,(hl)
	ld	(de),a
	inc	e
	inc	e
	inc	c
	ld	a,c
	cp	#64
	jr	nz,gfx_pairs_k0_loop
	ret

gfx_pairs_g7_kinds:
	ld	a,(gfx_e_fg)
	ld	d,a			; D = fg
	ld	a,(gfx_e_bg)
	ld	e,a			; E = bg
	ld	a,(_gfx_pair_page)
	ld	h,a
	ld	l,#0
	ld	a,(e_kind)
	cp	#2
	jr	z,gfx_pairs_k2
	; kind 3: 16 entries of four pixel bytes (bits 3..0)
	ld	c,#0
gfx_pairs_k3_loop:
	ld	a,c
	add	a,a
	add	a,a
	add	a,a
	add	a,a			; the index in bits 7..4
	call	gfx_pairs_g7_4
	inc	c
	ld	a,c
	cp	#16
	jr	nz,gfx_pairs_k3_loop
	ret
gfx_pairs_k2:
	; kind 2: 64 entries; pixels 0-3 (bits 5..2) on page A, 4-5 on page B
	ld	c,#0
gfx_pairs_k2_loop:
	ld	a,c
	add	a,a
	add	a,a			; the index in bits 7..2
	call	gfx_pairs_g7_4
	inc	h
	dec	l
	dec	l
	dec	l
	dec	l
	ld	a,c
	rrca
	rrca				; bits 1..0 to 7..6
	call	gfx_pairs_g7_2
	inc	l
	inc	l
	dec	h
	inc	c
	ld	a,c
	cp	#64
	jr	nz,gfx_pairs_k2_loop
	ret

; Four (gfx_pairs_g7_4) or two (gfx_pairs_g7_2) pixel bytes from A's top
; bits at (HL++): D for a set bit, E otherwise.  Keeps BC, DE.
gfx_pairs_g7_4:
	call	gfx_pairs_g7_2
gfx_pairs_g7_2:
	ld	(hl),e
	add	a,a
	jr	nc,gfx_pairs_g7_bit1
	ld	(hl),d
gfx_pairs_g7_bit1:
	inc	l
	ld	(hl),e
	add	a,a
	jr	nc,gfx_pairs_g7_bit2
	ld	(hl),d
gfx_pairs_g7_bit2:
	inc	l
	ret
