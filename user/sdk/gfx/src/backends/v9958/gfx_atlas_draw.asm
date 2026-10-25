; -----------------------------------------------------------------------------
; libgfx: transparent text from the glyph atlas (docs/gfx-sdk.md 14.1).
;
; gfx_text hands a GFX_TEXT_TRANSPARENT string here.  One atlas per text
; colour holds the range's glyphs in that colour on colour 0 (gfx_atlas.c
; lays the band out at gfx_text_atlas: the cell table and the slots);
; the colour's atlas is built on its first use, a full band rebuilding the
; least recently used colour's.  Each glyph is then one LMMM with TIMP
; from its cell, so colour-0 pixels leave the screen untouched; the copy's
; direction follows any overlap of the cell and the target (DIX/DIY with
; the far edges, as gfx_copy_rect).  A glyph redefined since the atlases
; drew it is redrawn into every atlas first, alone.
;
; An atlas build draws each atlas row of glyphs as one opaque string
; through the text emit (one VRAM address per scanline of the row).
; -----------------------------------------------------------------------------

	.module gfx_atlas_draw

	.globl gfx_atlas_draw
	.globl gfx_ad_x
	.globl gfx_ad_y
	.globl gfx_ad_ptr
	.globl gfx_ad_len
	.globl gfx_ad_fg

	.globl gfx_text_emit
	.globl gfx_fit
	.globl gfx_e_x
	.globl gfx_e_y
	.globl gfx_e_ptr
	.globl gfx_e_n
	.globl gfx_e_fg
	.globl gfx_e_bg
	.globl gfx_e_font
	.globl gfx_cmd_block
	.globl gfx_cmd_commit
	.globl gfx_wait_ce
	.globl gfx_ret_einval
	.globl _gfx_flush
	.globl _gfx_draw_mode
	.globl _gfx_draw_width

	; the atlas layout (gfx_atlas.c)
	.globl _gfx_atlas_active
	.globl _gfx_atlas_mode
	.globl _gfx_atlas_font
	.globl _gfx_atlas_gw
	.globl _gfx_atlas_ch
	.globl _gfx_atlas_first
	.globl _gfx_atlas_count
	.globl _gfx_atlas_slots
	.globl _gfx_atlas_lru_next
	.globl _gfx_atlas_per_row
	.globl _gfx_atlas_rows
	.globl _gfx_atlas_slot_color
	.globl _gfx_atlas_slot_lru
	.globl _gfx_atlas_slot_y
	.globl _gfx_atlas_cell_x
	.globl _gfx_atlas_cell_y
	.globl _gfx_font_stale6
	.globl _gfx_font_stale4

GFX_CMD_LMMM_TIMP       = 0x98
GFX_ARG_DIX             = 0x04
GFX_ARG_DIY             = 0x08
GFX_SLOT_FREE           = 0xFF

	.area _DATA

gfx_ad_x::
	.ds 2
gfx_ad_y::
	.ds 2
gfx_ad_ptr::
	.ds 2
gfx_ad_len::
	.ds 2
gfx_ad_fg::
	.ds 1

ad_fit:
	.ds 1
ad_slot:
	.ds 1
ad_slot_y:
	.ds 2
ad_dx:
	.ds 2
ad_outside:
	.ds 2
ad_code:
	.ds 1
ad_idx:
	.ds 1
ad_stale:
	.ds 2			; the font's stale-mark array
; the build
ab_slot:
	.ds 1
ab_row:
	.ds 1
ab_left:
	.ds 2
ab_base:
	.ds 2

	.area _CODE

; A code's bit in its stale-mark byte.
ad_bits:
	.db	0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80

; The identity codes 0..255: an atlas row's glyphs are a slice of it.
gfx_identity:
	.db	0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07
	.db	0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F
	.db	0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17
	.db	0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F
	.db	0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27
	.db	0x28,0x29,0x2A,0x2B,0x2C,0x2D,0x2E,0x2F
	.db	0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37
	.db	0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F
	.db	0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47
	.db	0x48,0x49,0x4A,0x4B,0x4C,0x4D,0x4E,0x4F
	.db	0x50,0x51,0x52,0x53,0x54,0x55,0x56,0x57
	.db	0x58,0x59,0x5A,0x5B,0x5C,0x5D,0x5E,0x5F
	.db	0x60,0x61,0x62,0x63,0x64,0x65,0x66,0x67
	.db	0x68,0x69,0x6A,0x6B,0x6C,0x6D,0x6E,0x6F
	.db	0x70,0x71,0x72,0x73,0x74,0x75,0x76,0x77
	.db	0x78,0x79,0x7A,0x7B,0x7C,0x7D,0x7E,0x7F
	.db	0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87
	.db	0x88,0x89,0x8A,0x8B,0x8C,0x8D,0x8E,0x8F
	.db	0x90,0x91,0x92,0x93,0x94,0x95,0x96,0x97
	.db	0x98,0x99,0x9A,0x9B,0x9C,0x9D,0x9E,0x9F
	.db	0xA0,0xA1,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7
	.db	0xA8,0xA9,0xAA,0xAB,0xAC,0xAD,0xAE,0xAF
	.db	0xB0,0xB1,0xB2,0xB3,0xB4,0xB5,0xB6,0xB7
	.db	0xB8,0xB9,0xBA,0xBB,0xBC,0xBD,0xBE,0xBF
	.db	0xC0,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7
	.db	0xC8,0xC9,0xCA,0xCB,0xCC,0xCD,0xCE,0xCF
	.db	0xD0,0xD1,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7
	.db	0xD8,0xD9,0xDA,0xDB,0xDC,0xDD,0xDE,0xDF
	.db	0xE0,0xE1,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7
	.db	0xE8,0xE9,0xEA,0xEB,0xEC,0xED,0xEE,0xEF
	.db	0xF0,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7
	.db	0xF8,0xF9,0xFA,0xFB,0xFC,0xFD,0xFE,0xFF

; ---------------------------------------------------------------------------
; gfx_atlas_draw: the string gfx_ad_ptr[0 .. gfx_ad_len - 1] at (gfx_ad_x,
; gfx_ad_y) in colour gfx_ad_fg.  DE = the count of glyphs outside the
; atlas range (they draw nothing), or -KOS_EINVAL.
; ---------------------------------------------------------------------------
gfx_atlas_draw::
	ld	a,(_gfx_atlas_active)
	or	a
	jp	z,gfx_ret_einval
	ld	a,(_gfx_draw_mode)
	ld	hl,#_gfx_atlas_mode
	cp	(hl)
	jp	nz,gfx_ret_einval
	ld	hl,(_gfx_draw_width)
	ld	de,(gfx_ad_x)
	or	a
	sbc	hl,de			; the room
	jp	c,gfx_ret_einval
	jp	z,gfx_ret_einval
	push	hl
	call	_gfx_flush
	call	gfx_wait_ce
	pop	hl
	ld	a,(_gfx_atlas_gw)
	ld	c,a
	ld	de,(gfx_ad_len)
	call	gfx_fit
	ld	(ad_fit),a
	ld	hl,#_gfx_font_stale6
	ld	a,(_gfx_atlas_font)
	or	a
	jr	z,ad_stale_set
	ld	hl,#_gfx_font_stale4
ad_stale_set:
	ld	(ad_stale),hl
	call	ad_slot_for
	ld	(ad_slot),a
	add	a,a
	ld	l,a
	ld	h,#0
	ld	de,#_gfx_atlas_slot_y
	add	hl,de
	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	ld	(ad_slot_y),de
	ld	hl,#0
	ld	(ad_outside),hl
	ld	hl,(gfx_ad_x)
	ld	(ad_dx),hl
	; the copies' constant fields: one cell, colour 0, LMMM with TIMP
	ld	a,(_gfx_atlas_gw)
	ld	l,a
	ld	h,#0
	ld	(gfx_cmd_block+8),hl	; NX
	ld	a,(_gfx_atlas_ch)
	ld	l,a
	ld	(gfx_cmd_block+10),hl	; NY
	xor	a
	ld	(gfx_cmd_block+12),a	; CLR
	ld	a,#GFX_CMD_LMMM_TIMP
	ld	(gfx_cmd_block+14),a
	ld	a,(ad_fit)
	or	a
	jp	z,ad_done
ad_glyph:
	ld	hl,(gfx_ad_ptr)
	ld	a,(hl)
	inc	hl
	ld	(gfx_ad_ptr),hl
	ld	(ad_code),a
	ld	hl,#_gfx_atlas_first
	sub	(hl)
	jp	c,ad_outside_glyph	; below the range
	ld	(ad_idx),a
	ld	hl,(_gfx_atlas_count)
	ld	b,a
	ld	a,h
	or	a
	jr	nz,ad_inside		; a 256-glyph range holds every code
	ld	a,b
	cp	l
	jp	nc,ad_outside_glyph
ad_inside:
	; a stale glyph is redrawn into every atlas first
	ld	a,(ad_code)
	ld	b,a
	rrca
	rrca
	rrca
	and	#0x1F
	ld	e,a
	ld	d,#0
	ld	hl,(ad_stale)
	add	hl,de			; HL = the mark byte
	ld	a,b
	and	#7
	push	hl
	ld	hl,#ad_bits
	add	a,l
	ld	l,a
	ld	a,#0
	adc	a,h
	ld	h,a
	ld	a,(hl)
	pop	hl
	ld	c,a			; C = the code's bit
	and	(hl)
	jr	z,ad_cell
	ld	a,c
	cpl
	and	(hl)
	ld	(hl),a			; cleared
	call	ad_redraw_stale
ad_cell:
	; SY = slot_y + cell_y[idx], SX = cell_x[idx]
	ld	a,(ad_idx)
	ld	c,a
	ld	b,#0
	ld	hl,#_gfx_atlas_cell_y
	add	hl,bc
	ld	a,(hl)
	ld	hl,(ad_slot_y)
	add	a,l
	ld	l,a
	ld	a,#0
	adc	a,h
	ld	h,a
	push	hl			; sy
	ld	hl,#_gfx_atlas_cell_x
	add	hl,bc
	add	hl,bc
	ld	e,(hl)
	inc	hl
	ld	d,(hl)			; DE = sx
	ld	b,#0			; B = ARG
	; X: DIX with the far edges when the cell sits left of the target
	ld	hl,(ad_dx)
	ex	de,hl			; HL = sx, DE = dx
	or	a
	sbc	hl,de
	add	hl,de
	jr	nc,ad_x_fwd
	ld	b,#GFX_ARG_DIX
	ld	a,(_gfx_atlas_gw)
	dec	a
	ld	c,a
	push	bc
	ld	b,#0
	add	hl,bc
	ex	de,hl
	add	hl,bc
	ex	de,hl
	pop	bc
ad_x_fwd:
	ld	(gfx_cmd_block+0),hl	; SX
	ld	(gfx_cmd_block+4),de	; DX
	pop	hl			; sy
	ld	de,(gfx_ad_y)
	or	a
	sbc	hl,de
	add	hl,de
	jr	nc,ad_y_fwd
	ld	a,b
	or	#GFX_ARG_DIY
	ld	b,a
	ld	a,(_gfx_atlas_ch)
	dec	a
	ld	c,a
	push	bc
	ld	b,#0
	add	hl,bc
	ex	de,hl
	add	hl,bc
	ex	de,hl
	pop	bc
ad_y_fwd:
	ld	(gfx_cmd_block+2),hl	; SY
	ld	(gfx_cmd_block+6),de	; DY
	ld	a,b
	ld	(gfx_cmd_block+13),a	; ARG
	call	gfx_cmd_commit
	jr	ad_next
ad_outside_glyph:
	ld	hl,(ad_outside)
	inc	hl
	ld	(ad_outside),hl
ad_next:
	ld	hl,(ad_dx)
	ld	a,(_gfx_atlas_gw)
	add	a,l
	ld	l,a
	ld	a,#0
	adc	a,h
	ld	h,a
	ld	(ad_dx),hl
	ld	hl,#ad_fit
	dec	(hl)
	jp	nz,ad_glyph
ad_done:
	; the slot is the most recently used
	ld	a,(ad_slot)
	ld	e,a
	ld	d,#0
	ld	hl,#_gfx_atlas_slot_lru
	add	hl,de
	ld	a,(_gfx_atlas_lru_next)
	inc	a
	ld	(_gfx_atlas_lru_next),a
	ld	(hl),a
	ld	de,(ad_outside)
	ret

; ---------------------------------------------------------------------------
; ad_slot_for: A = the slot of gfx_ad_fg: its own, else the first free one,
; else the least recently used one; a slot taken for the colour is built.
; ---------------------------------------------------------------------------
ad_slot_for:
	ld	a,(_gfx_atlas_slots)
	ld	b,a
	ld	c,#0			; C = i
	ld	d,#GFX_SLOT_FREE	; D = the first free slot
	ld	e,#0			; E = the oldest
ad_slot_loop:
	ld	a,c
	cp	b
	jr	nc,ad_slot_pick
	ld	hl,#_gfx_atlas_slot_color
	call	ad_index		; HL = &slot_color[i]
	ld	a,(gfx_ad_fg)
	cp	(hl)
	ld	a,c
	ret	z			; the colour's own slot
	ld	a,(hl)
	cp	#GFX_SLOT_FREE
	jr	nz,ad_slot_lru
	ld	a,d
	cp	#GFX_SLOT_FREE
	jr	nz,ad_slot_lru
	ld	d,c
ad_slot_lru:
	; oldest = i when lru[i] < lru[oldest] or slot[oldest] is free
	push	de
	ld	hl,#_gfx_atlas_slot_color
	ld	a,e
	call	ad_index_a
	ld	a,(hl)
	cp	#GFX_SLOT_FREE
	jr	z,ad_slot_take
	ld	hl,#_gfx_atlas_slot_lru
	ld	a,e
	call	ad_index_a
	ld	e,(hl)			; lru[oldest]
	ld	hl,#_gfx_atlas_slot_lru
	call	ad_index
	ld	a,(hl)			; lru[i]
	cp	e
	jr	c,ad_slot_take
	pop	de
	jr	ad_slot_next
ad_slot_take:
	pop	de
	ld	e,c
ad_slot_next:
	inc	c
	jr	ad_slot_loop
ad_slot_pick:
	ld	a,d
	cp	#GFX_SLOT_FREE
	jr	nz,ad_slot_build
	ld	a,e
ad_slot_build:
	push	af
	call	ad_build
	pop	af
	ret

; HL += C (ad_index) or A (ad_index_a).  Keeps BC, DE.
ad_index:
	ld	a,c
ad_index_a:
	add	a,l
	ld	l,a
	ld	a,#0
	adc	a,h
	ld	h,a
	ret

; ---------------------------------------------------------------------------
; ad_build: A = the slot; draw the range in gfx_ad_fg on colour 0, one
; opaque string per atlas row.
; ---------------------------------------------------------------------------
ad_build:
	ld	(ab_slot),a
	add	a,a
	ld	l,a
	ld	h,#0
	ld	de,#_gfx_atlas_slot_y
	add	hl,de
	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	ld	(ab_base),de		; the row's y
	ld	hl,(_gfx_atlas_count)
	ld	(ab_left),hl
	ld	hl,#gfx_identity
	ld	a,(_gfx_atlas_first)
	ld	e,a
	ld	d,#0
	add	hl,de
	ld	(gfx_e_ptr),hl
	ld	a,(gfx_ad_fg)
	ld	(gfx_e_fg),a
	xor	a
	ld	(gfx_e_bg),a
	ld	hl,#0
	ld	(gfx_e_x),hl
	ld	a,(_gfx_atlas_font)
	ld	(gfx_e_font),a
	ld	a,(_gfx_atlas_rows)
	ld	(ab_row),a
ad_build_row:
	; n = min(per_row, left)
	ld	a,(_gfx_atlas_per_row)
	ld	e,a
	ld	d,#0
	ld	hl,(ab_left)
	or	a
	sbc	hl,de
	jr	nc,ad_build_full
	ld	a,(ab_left)		; the last, short row
	ld	hl,#0
	jr	ad_build_n
ad_build_full:
	ld	a,e
ad_build_n:
	ld	(ab_left),hl
	ld	(gfx_e_n),a
	ld	hl,(ab_base)
	ld	(gfx_e_y),hl
	call	gfx_text_emit
	; the next row: the codes and y move on
	ld	a,(_gfx_atlas_per_row)
	ld	e,a
	ld	d,#0
	ld	hl,(gfx_e_ptr)
	add	hl,de
	ld	(gfx_e_ptr),hl
	ld	a,(_gfx_atlas_ch)
	ld	e,a
	ld	hl,(ab_base)
	add	hl,de
	ld	(ab_base),hl
	ld	hl,#ab_row
	dec	(hl)
	jr	nz,ad_build_row
	; the slot now holds the colour
	ld	a,(ab_slot)
	ld	e,a
	ld	d,#0
	ld	hl,#_gfx_atlas_slot_color
	add	hl,de
	ld	a,(gfx_ad_fg)
	ld	(hl),a
	ret

; ---------------------------------------------------------------------------
; ad_redraw_stale: the glyph ad_code (index ad_idx) redrawn into every
; built atlas, one cell each.
; ---------------------------------------------------------------------------
ad_redraw_stale:
	ld	hl,#gfx_identity
	ld	a,(ad_code)
	ld	e,a
	ld	d,#0
	add	hl,de
	ld	(gfx_e_ptr),hl
	ld	a,#1
	ld	(gfx_e_n),a
	xor	a
	ld	(gfx_e_bg),a
	ld	a,(_gfx_atlas_font)
	ld	(gfx_e_font),a
	; x = cell_x[idx]
	ld	a,(ad_idx)
	ld	l,a
	ld	h,#0
	add	hl,hl
	ld	de,#_gfx_atlas_cell_x
	add	hl,de
	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	ld	(gfx_e_x),de
	ld	c,#0
ad_redraw_loop:
	ld	a,(_gfx_atlas_slots)
	cp	c
	ret	z
	ret	c
	ld	hl,#_gfx_atlas_slot_color
	call	ad_index
	ld	a,(hl)
	cp	#GFX_SLOT_FREE
	jr	z,ad_redraw_next
	ld	(gfx_e_fg),a
	; y = slot_y[i] + cell_y[idx]
	ld	a,c
	add	a,a
	ld	l,a
	ld	h,#0
	ld	de,#_gfx_atlas_slot_y
	add	hl,de
	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	ld	hl,#_gfx_atlas_cell_y
	ld	a,(ad_idx)
	call	ad_index_a
	ld	l,(hl)
	ld	h,#0
	add	hl,de
	ld	(gfx_e_y),hl
	push	bc
	call	gfx_text_emit
	pop	bc
ad_redraw_next:
	inc	c
	jr	ad_redraw_loop
