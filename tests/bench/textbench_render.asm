; KonaOS direct-text spike renderers (textbench.asm).
;
; Four direct-write text renderers, each with the full text model (CR,
; LF, wrap at 85, circular scrolling through the C helper
; `textbench_scroll`, the console's own algorithm).  The C part fills
; the pointers and the model state; each entry renders
; `textbench_text` of `textbench_text_len` bytes from the current
; (x, y) at the current origin:
;
;   textbench_render_glyph1  per glyph, eight scanlines of "set the
;                            VRAM address, three OUT (A0h)", the bytes
;                            expanded from the 1-bit font through the
;                            64-entry, 3-byte `textbench_expand` table;
;   textbench_render_glyph6  the same shape from the pre-coloured
;                            `textbench_font6` (24 bytes per glyph)
;                            through OUTI, no expansion;
;   textbench_render_run1    per run (printables up to a control byte
;                            or the right margin): one address set per
;                            scanline, then three bytes per glyph of
;                            the run, 1-bit expanded;
;   textbench_render_run6    as run1 from the pre-coloured font.
;
; Conventions: GRAPHIC 6 (6 pixels per cell = 3 whole bytes per
; scanline, 256 bytes per line); the cell at (x, y) lives at
; ((origin + y*8 + scan) & 0FFh) * 256 + x*3.  The data port
; auto-increments the VRAM address, so within one scanline only one
; address set is needed; the address itself is kept in a working cell.
; Page zero is selected by R#14 = 0 (set by the clear).  The rows
; render normal attribute only, so the reverse XOR the console applies
; has no effect here.

	.module textbench_render

	.globl _textbench_render_glyph1
	.globl _textbench_render_glyph1_end
	.globl _textbench_render_glyph6
	.globl _textbench_render_glyph6_end
	.globl _textbench_render_run1
	.globl _textbench_render_run1_end
	.globl _textbench_render_run6
	.globl _textbench_render_run6_end
	.globl _textbench_shared
	.globl _textbench_shared_end

	.globl _textbench_x
	.globl _textbench_y
	.globl _textbench_origin
	.globl _textbench_text
	.globl _textbench_text_len
	.globl _textbench_expand
	.globl _textbench_font
	.globl _textbench_font6
	.globl _textbench_scroll
	.globl _textbench_page

	.area _CODE

; ---------------------------------------------------------------------------
; The shared model pieces.
; ---------------------------------------------------------------------------

_textbench_shared::

; Row + 1, or one cell's scroll at the bottom.  Preserves HL and DE
; (the model's text pointer and remaining count).
newline_check:
	push	hl
	push	de
	ld	a,(_textbench_y)
	inc	a
	cp	#26
	jr	nz,nc_ok
	call	_textbench_scroll
	pop	de
	pop	hl
	ret
nc_ok:
	ld	(_textbench_y),a
	pop	de
	pop	hl
	ret

; The glyph rows' model head: HL = text pointer, DE = remaining count,
; Z set when the text is empty.
model_glyph_head:
	ld	hl,(_textbench_text)
	ld	de,(_textbench_text_len)
	ld	a,d
	or	e
	ret

; The run rows' model head: as above, with an empty run.
model_run_head:
	ld	hl,(_textbench_text)
	ld	de,(_textbench_text_len)
	xor	a
	ld	(_textbench_run_len),a
	ld	a,d
	or	e
	ret

; The base address of the cell/run at column `col` (the `x` argument)
; into the working address cell: (origin + y*8) * 256 + col*3.
; A = col on entry.
cell_base:
	ld	c,a
	ld	a,(_textbench_origin)
	ld	b,a
	ld	a,(_textbench_y)
	add	a,a
	add	a,a
	add	a,a
	add	a,b			; origin + y*8
	ld	(_textbench_addr+1),a
	xor	a
	ld	(_textbench_addr),a
	ld	a,c
	ld	b,a
	add	a,a
	add	a,b			; col*3
	ld	hl,(_textbench_addr)
	add	a,l
	ld	(_textbench_addr),a
	ret

; One address set from the working cell, advanced to the next scanline
; (256 bytes).  Preserves DE.  The V9958's 17-bit address splits across
; R#14 (A16:A14) and the command-port pair (A13:A0), so the page
; (y >> 6) is written to R#14 when it changes and the high byte carries
; y & 3Fh.
address_set_next:
	ld	hl,(_textbench_addr)
	ld	a,h
	rlca
	rlca
	and	#0x03			; the page: y >> 6
	ld	c,a
	ld	a,(_textbench_page)
	cp	c
	jr	z,asn_page_ok
	ld	a,c
	ld	(_textbench_page),a
	out	(0xA1),a		; R#14 = the page
	ld	a,#(0x80 | 14)
	out	(0xA1),a
asn_page_ok:
	ld	a,l
	out	(0xA1),a
	ld	a,h
	and	#0x3F
	or	#0x40
	out	(0xA1),a
	inc	h
	ld	(_textbench_addr),hl
	ret

_textbench_shared_end::

; ---------------------------------------------------------------------------
; txt.glyph1: per glyph, 8 scanlines, 3 expanded bytes each.
; ---------------------------------------------------------------------------
_textbench_render_glyph1::
	call	model_glyph_head
	ret	z
g1_loop:
	ld	a,(hl)
	inc	hl
	cp	#0x0D
	jr	z,g1_cr
	cp	#0x0A
	jr	z,g1_lf
	; printable: one glyph at (x, y)
	push	hl
	call	glyph1_cell
	pop	hl
	ld	a,(_textbench_x)
	inc	a
	cp	#85
	jr	nz,g1_adv
	xor	a
	ld	(_textbench_x),a
	call	newline_check
	jr	g1_count
g1_adv:
	ld	(_textbench_x),a
g1_count:
	dec	de
	ld	a,d
	or	e
	jr	nz,g1_loop
	ret
g1_cr:
	xor	a
	ld	(_textbench_x),a
	jr	g1_count
g1_lf:
	call	newline_check
	jr	g1_count
_textbench_render_glyph1_end::

; One glyph, expanded from the 1-bit font.  A = character code.
; Preserves DE (the model's remaining count).
glyph1_cell:
	push	de
	; DE = font + ch*8
	ld	l,a
	ld	h,#0
	add	hl,hl
	add	hl,hl
	add	hl,hl
	ld	de,#_textbench_font
	add	hl,de
	ex	de,hl
	; the cell's base address
	ld	a,(_textbench_x)
	call	cell_base
	ld	a,#8
	ld	(_textbench_scan),a
g1_scan:
	call	address_set_next
	; one font row byte -> the 3-byte expansion entry
	ld	a,(de)
	inc	de
	srl	a
	srl	a
	ld	l,a
	ld	h,#0
	ld	c,a
	ld	b,#0
	add	hl,hl
	add	hl,bc
	ld	bc,#_textbench_expand
	add	hl,bc
	ld	a,(hl)
	out	(0xA0),a
	inc	hl
	ld	a,(hl)
	out	(0xA0),a
	inc	hl
	ld	a,(hl)
	out	(0xA0),a
	ld	a,(_textbench_scan)
	dec	a
	ld	(_textbench_scan),a
	jr	nz,g1_scan
	pop	de
	ret

; ---------------------------------------------------------------------------
; txt.glyph6: per glyph, 8 scanlines, 3 pre-coloured bytes each (OUTI).
; ---------------------------------------------------------------------------
_textbench_render_glyph6::
	call	model_glyph_head
	ret	z
g6_loop:
	ld	a,(hl)
	inc	hl
	cp	#0x0D
	jr	z,g6_cr
	cp	#0x0A
	jr	z,g6_lf
	push	hl
	call	glyph6_cell
	pop	hl
	ld	a,(_textbench_x)
	inc	a
	cp	#85
	jr	nz,g6_adv
	xor	a
	ld	(_textbench_x),a
	call	newline_check
	jr	g6_count
g6_adv:
	ld	(_textbench_x),a
g6_count:
	dec	de
	ld	a,d
	or	e
	jr	nz,g6_loop
	ret
g6_cr:
	xor	a
	ld	(_textbench_x),a
	jr	g6_count
g6_lf:
	call	newline_check
	jr	g6_count
_textbench_render_glyph6_end::

; One glyph from the pre-coloured font.  A = character code.
; Preserves DE (the model's remaining count).
glyph6_cell:
	push	de
	; HL = font6 + ch*24 (the OUTI source, advanced by the OUTIs)
	ld	l,a
	ld	h,#0
	add	hl,hl
	add	hl,hl
	add	hl,hl
	ld	b,h
	ld	c,l
	add	hl,hl
	add	hl,bc
	ld	de,#_textbench_font6
	add	hl,de
	ld	a,(_textbench_x)
	push	hl
	call	cell_base
	pop	hl
	ld	a,#8
	ld	(_textbench_scan),a
g6_scan:
	push	hl
	call	address_set_next
	pop	hl
	ld	b,#3
	ld	c,#0xA0
	outi
	outi
	outi
	ld	a,(_textbench_scan)
	dec	a
	ld	(_textbench_scan),a
	jr	nz,g6_scan
	pop	de
	ret

; ---------------------------------------------------------------------------
; txt.run1: per run, one address set per scanline, 3 expanded bytes per
; glyph.  The model collects the run (text pointer, start column,
; length) and flushes it on CR, LF, the right margin and the end of the
; text.
; ---------------------------------------------------------------------------
_textbench_render_run1::
	call	model_run_head
	ret	z
r1_loop:
	ld	a,(hl)
	inc	hl
	cp	#0x0D
	jr	z,r1_cr
	cp	#0x0A
	jr	z,r1_lf
	; printable; consume a pending margin wrap first
	ld	a,(_textbench_x)
	cp	#85
	jr	nz,r1_append
	push	hl
	call	flush_run1
	pop	hl
	push	hl
	call	newline_check
	pop	hl
	xor	a
	ld	(_textbench_x),a
r1_append:
	ld	a,(_textbench_run_len)
	or	a
	jr	nz,r1_run_ok
	dec	hl
	ld	(_textbench_run_ptr),hl
	inc	hl
	ld	a,(_textbench_x)
	ld	(_textbench_run_x),a
r1_run_ok:
	ld	a,(_textbench_run_len)
	inc	a
	ld	(_textbench_run_len),a
	ld	a,(_textbench_x)
	inc	a
	ld	(_textbench_x),a
r1_count:
	dec	de
	ld	a,d
	or	e
	jr	nz,r1_loop
	jp	flush_run1
r1_cr:
	push	hl
	call	flush_run1
	pop	hl
	ld	a,(_textbench_x)
	cp	#85
	jr	nz,r1_cr_col
	push	hl
	call	newline_check
	pop	hl
r1_cr_col:
	xor	a
	ld	(_textbench_x),a
	jr	r1_count
r1_lf:
	push	hl
	call	flush_run1
	pop	hl
	ld	a,(_textbench_x)
	cp	#85
	jr	nz,r1_lf_nl
	push	hl
	call	newline_check
	pop	hl
	xor	a
	ld	(_textbench_x),a
r1_lf_nl:
	push	hl
	call	newline_check
	pop	hl
	jr	r1_count
_textbench_render_run1_end::

; Render the collected run (no-op when empty).  Preserves HL.
flush_run1:
	ld	a,(_textbench_run_len)
	or	a
	ret	z
	push	de
	ld	a,(_textbench_run_x)
	call	cell_base
	ld	a,#8
	ld	(_textbench_scan),a
f1_scan:
	call	address_set_next
	ld	de,(_textbench_run_ptr)
	ld	a,(_textbench_run_len)
	ld	b,a
f1_glyph:
	push	bc
	ld	a,(de)
	inc	de
	; font + ch*8 + scan (scan = 8 - the counter)
	ld	l,a
	ld	h,#0
	add	hl,hl
	add	hl,hl
	add	hl,hl
	ld	bc,#_textbench_font
	add	hl,bc
	ld	a,(_textbench_scan)
	ld	c,a
	ld	a,#8
	sub	c
	ld	c,a
	ld	b,#0
	add	hl,bc
	ld	a,(hl)
	srl	a
	srl	a
	ld	l,a
	ld	h,#0
	ld	c,a
	ld	b,#0
	add	hl,hl
	add	hl,bc
	ld	bc,#_textbench_expand
	add	hl,bc
	ld	a,(hl)
	out	(0xA0),a
	inc	hl
	ld	a,(hl)
	out	(0xA0),a
	inc	hl
	ld	a,(hl)
	out	(0xA0),a
	pop	bc
	djnz	f1_glyph
	ld	a,(_textbench_scan)
	dec	a
	ld	(_textbench_scan),a
	jr	nz,f1_scan
	xor	a
	ld	(_textbench_run_len),a
	pop	de
	ret

; ---------------------------------------------------------------------------
; txt.run6: per run, one address set per scanline, 3 pre-coloured bytes
; per glyph (OUTI).
; ---------------------------------------------------------------------------
_textbench_render_run6::
	call	model_run_head
	ret	z
r6_loop:
	ld	a,(hl)
	inc	hl
	cp	#0x0D
	jr	z,r6_cr
	cp	#0x0A
	jr	z,r6_lf
	ld	a,(_textbench_x)
	cp	#85
	jr	nz,r6_append
	push	hl
	call	flush_run6
	pop	hl
	push	hl
	call	newline_check
	pop	hl
	xor	a
	ld	(_textbench_x),a
r6_append:
	ld	a,(_textbench_run_len)
	or	a
	jr	nz,r6_run_ok
	dec	hl
	ld	(_textbench_run_ptr),hl
	inc	hl
	ld	a,(_textbench_x)
	ld	(_textbench_run_x),a
r6_run_ok:
	ld	a,(_textbench_run_len)
	inc	a
	ld	(_textbench_run_len),a
	ld	a,(_textbench_x)
	inc	a
	ld	(_textbench_x),a
r6_count:
	dec	de
	ld	a,d
	or	e
	jr	nz,r6_loop
	jp	flush_run6
r6_cr:
	push	hl
	call	flush_run6
	pop	hl
	ld	a,(_textbench_x)
	cp	#85
	jr	nz,r6_cr_col
	push	hl
	call	newline_check
	pop	hl
r6_cr_col:
	xor	a
	ld	(_textbench_x),a
	jr	r6_count
r6_lf:
	push	hl
	call	flush_run6
	pop	hl
	ld	a,(_textbench_x)
	cp	#85
	jr	nz,r6_lf_nl
	push	hl
	call	newline_check
	pop	hl
	xor	a
	ld	(_textbench_x),a
r6_lf_nl:
	push	hl
	call	newline_check
	pop	hl
	jr	r6_count
_textbench_render_run6_end::

; Render the collected run from the pre-coloured font (no-op when
; empty).  Preserves HL.
flush_run6:
	ld	a,(_textbench_run_len)
	or	a
	ret	z
	push	de
	ld	a,(_textbench_run_x)
	call	cell_base
	ld	a,#8
	ld	(_textbench_scan),a
f6_scan:
	call	address_set_next
	ld	de,(_textbench_run_ptr)
	ld	a,(_textbench_run_len)
	ld	b,a
f6_glyph:
	push	bc
	ld	a,(de)
	inc	de
	; font6 + ch*24 + scan*3 (scan = 8 - the counter)
	ld	l,a
	ld	h,#0
	add	hl,hl
	add	hl,hl
	add	hl,hl
	ld	b,h
	ld	c,l
	add	hl,hl
	add	hl,bc
	ld	bc,#_textbench_font6
	add	hl,bc
	ld	a,(_textbench_scan)
	ld	c,a
	ld	a,#8
	sub	c
	ld	c,a
	add	a,a
	add	a,c
	ld	c,a
	ld	b,#0
	add	hl,bc
	ld	b,#3
	ld	c,#0xA0
	outi
	outi
	outi
	pop	bc
	djnz	f6_glyph
	ld	a,(_textbench_scan)
	dec	a
	ld	(_textbench_scan),a
	jr	nz,f6_scan
	xor	a
	ld	(_textbench_run_len),a
	pop	de
	ret

; ---------------------------------------------------------------------------
; The model's working cells.
; ---------------------------------------------------------------------------
	.area _DATA
_textbench_scan::
	.ds	1
_textbench_addr::
	.ds	2
_textbench_run_ptr::
	.ds	2
_textbench_run_x::
	.ds	1
_textbench_run_len::
	.ds	1

; ---------------------------------------------------------------------------
; The pinned CP850 6x8 font, assembled from the driver's reviewable asset
; (drivers/v9958/assets/font_cp850_6x8.inc, the same source tools/fontpack.py
; packs): 8 bytes per glyph.
; ---------------------------------------------------------------------------
	.area _CODE
_textbench_font::
	.include "font_cp850_6x8.inc"
