; -----------------------------------------------------------------------------
; libsnd output engine. Manual voices write through their descriptors: the
; put primitives update the desired state, compare the shadow and emit at
; once (DIRECT: OUT (C),A to the descriptor's port; BATCH: an append).
; Tick-end work is only an invalid-shadow rewrite and a BATCH send.
;
; The descriptors are one 256-byte-aligned page (16 x 16 bytes, see
; snd_voice.asm and snd_private.h); a voice's attenuation (desired,
; shadow, latch) and port sit inside its descriptor, so the attenuation
; write walks them with `inc l`.  Periods and the noise control keep
; their per-chip arrays, reached through the descriptor's pointers.
;
; Internal fast ABI (also used by the tracker renderer):
;   HL = the voice's descriptor base (L = voice << 4)
;   snd_put_period: DE = 10-bit period
;   snd_put_atten:  A = hardware attenuation (0..15)
;   snd_put_noise:  A = E0h control byte, B != 0 forces an equal write
; Each clobbers A, BC, DE and L (H, the descriptor page, is kept).
; -----------------------------------------------------------------------------

	.module snd_out

	.globl _snd_load_ports
	.globl _snd_shadow_invalidate
	.globl _snd_output_flush
	.globl _snd_noise_apply
	.globl snd_put_period
	.globl snd_put_atten
	.globl snd_put_noise
	.globl snd_batch_send
	.globl _sound_write
	.globl _snd_handle
	.globl _snd_output_mode
	.globl _snd_dirty
	.globl _snd_voice_work
	.globl _snd_due_work
	.globl _snd_desc_page
	.globl _snd_all_valid

SND_CHIP_COUNT   = 4
SND_BATCH_MAX    = 60
SND_P_NOISE_BYTE = 0xE0

; Descriptor fields (mirrored in snd_voice.asm and snd_private.h).
VD_ATT_DESIRED   = 6
VD_ATT_SHADOW    = 7
VD_ATT_LATCH     = 8
VD_PORT          = 9
VD_REG_LATCH     = 10
VD_CHIP          = 11
VD_DESIRED_REG   = 12
VD_SHADOW_REG    = 14

	.area _DATA

_snd_ports::
	.ds SND_CHIP_COUNT
_snd_desired_period::
	.ds SND_CHIP_COUNT * 3 * 2
_snd_desired_noise::
	.ds SND_CHIP_COUNT
_snd_shadow_period::
	.ds SND_CHIP_COUNT * 3 * 2
_snd_shadow_noise::
	.ds SND_CHIP_COUNT
_snd_shadow_valid::
	.ds SND_CHIP_COUNT
_snd_all_valid::
	.ds 1			; 1 while every chip's shadow is valid
_snd_batch::
	.ds SND_BATCH_MAX * 2
_snd_batch_count::
	.ds 1
_snd_dirty::
	.ds 1

; Full-rewrite working bytes are off the steady write-through path.
_snd_rw_chip::
	.ds 1
_snd_rw_port::
	.ds 1
_snd_rw_latch::
	.ds 1

	.area _CODE

_snd_load_ports::
	ld	de, #_snd_ports
	ld	bc, #0x0404		; B = four ports, C = info offset
1$:
	push	bc
	push	hl
	ld	a, l
	add	a, c
	ld	l, a
	jr	nc, 2$
	inc	h
2$:
	ld	a, (hl)
	ld	(de), a
	pop	hl
	pop	bc
	inc	c
	inc	de
	djnz	1$
	ret

; Unknown hardware, a muted desired state: the next flush rewrites all.
_snd_shadow_invalidate::
	xor	a
	ld	(_snd_all_valid), a
	ld	hl, #_snd_shadow_valid
	ld	b, #SND_CHIP_COUNT
1$:
	ld	(hl), a
	inc	hl
	djnz	1$
	; every descriptor: desired attenuation 15 (silent), no forced write
	ld	a, (_snd_desc_page)
	ld	h, a
	ld	l, #0
2$:
	res	2, (hl)			; VF_FORCE
	ld	a, l
	or	#VD_ATT_DESIRED
	ld	l, a
	ld	(hl), #0x0F
	ld	a, l
	and	#0xF0
	add	a, #16
	ld	l, a
	jr	nz, 2$
	ld	hl, #_snd_desired_noise
	ld	b, #SND_CHIP_COUNT
	ld	a, #SND_P_NOISE_BYTE
3$:
	ld	(hl), a
	inc	hl
	djnz	3$
	ld	a, #1
	ld	(_snd_dirty), a
	ret

; Foreground control publishes desired+force. The voice-order walk applies it
; through the chip's noise descriptor inside the next tick.
_snd_noise_apply::
	ld	c, l
	srl	a
	srl	a
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_desired_noise
	add	hl, de
	ld	a, c
	or	#SND_P_NOISE_BYTE
	ld	(hl), a
	; the chip's noise descriptor (voice chip*4+3) carries the force
	ld	a, e
	rrca
	rrca
	or	#0x30
	ld	l, a
	ld	a, (_snd_desc_page)
	ld	h, a
	set	2, (hl)			; VF_FORCE
	ld	a, #1
	ld	(_snd_voice_work), a
	ld	(_snd_due_work), a
	ret

; ---------------------------------------------------------------------------
; The emit for a descriptor: A = the byte, L = any field of the
; descriptor (its base is L & F0h).  DIRECT: OUT to the descriptor's
; port; BATCH: an append, a full batch sent first.  Clobbers A, BC, DE;
; keeps HL.
; ---------------------------------------------------------------------------
_desc_emit:
	ld	b, a
	ld	a, l
	and	#0xF0
	or	#VD_PORT
	ld	e, l			; keep L
	ld	l, a
	ld	a, (_snd_output_mode)
	or	a
	jr	nz, _desc_batch
	ld	c, (hl)			; the port
	ld	l, e
	out	(c), b
	ret
_desc_batch:
	inc	l
	inc	l			; VD_CHIP
	ld	c, (hl)
	ld	l, e
	push	hl
	ld	a, (_snd_batch_count)
	cp	#SND_BATCH_MAX
	jr	c, 1$
	push	bc
	call	snd_batch_send
	pop	bc
1$:
	ld	a, (_snd_batch_count)
	ld	e, a
	inc	a
	ld	(_snd_batch_count), a
	ld	d, #0
	ld	hl, #_snd_batch
	add	hl, de
	add	hl, de
	ld	(hl), c			; the chip
	inc	hl
	ld	(hl), b			; the byte
	pop	hl
	ret

; The slow validity check: Z when the descriptor's chip shadow is
; invalid (L = any field).  Keeps BC, HL.
_chip_invalid:
	push	hl
	ld	a, l
	and	#0xF0
	or	#VD_CHIP
	ld	l, a
	ld	e, (hl)
	ld	d, #0
	ld	hl, #_snd_shadow_valid
	add	hl, de
	ld	a, (hl)
	pop	hl
	or	a
	ret

; ---------------------------------------------------------------------------
; snd_put_atten: HL = descriptor base, A = attenuation 0..15.
; ---------------------------------------------------------------------------
snd_put_atten::
	ld	c, a
	ld	a, l
	and	#0xF0
	or	#VD_ATT_DESIRED
	ld	l, a
	ld	(hl), c			; desired
	inc	l			; shadow
	ld	a, (_snd_all_valid)
	or	a
	jr	z, _atten_check
_atten_valid:
	ld	a, c
	cp	(hl)
	ret	z			; unchanged
	ld	(hl), a			; shadow
	inc	l			; latch
	or	(hl)
	ld	b, a
	ld	a, (_snd_output_mode)
	or	a
	jr	nz, _atten_batch
	inc	l			; port
	ld	c, (hl)
	out	(c), b
	ret
_atten_batch:
	ld	a, b
	jp	_desc_emit
_atten_check:
	call	_chip_invalid
	ret	z
	jr	_atten_valid

; ---------------------------------------------------------------------------
; snd_put_period: HL = descriptor base, DE = the 10-bit period.
; ---------------------------------------------------------------------------
snd_put_period::
	ld	a, l
	and	#0xF0
	or	#VD_DESIRED_REG
	ld	l, a
	ld	c, (hl)
	inc	l
	ld	b, (hl)			; BC = &desired
	ld	a, e
	ld	(bc), a
	inc	bc
	ld	a, d
	ld	(bc), a
	ld	a, (_snd_all_valid)
	or	a
	jr	nz, 1$
	push	de
	call	_chip_invalid
	pop	de
	ret	z
1$:
	inc	l			; VD_SHADOW_REG
	ld	c, (hl)
	inc	l
	ld	b, (hl)			; BC = &shadow
	ld	a, (bc)
	cp	e
	jr	nz, 2$
	inc	bc
	ld	a, (bc)
	cp	d
	ret	z
	dec	bc
2$:
	ld	a, e
	ld	(bc), a
	inc	bc
	ld	a, d
	ld	(bc), a
	ld	a, l
	and	#0xF0
	or	#VD_PORT
	ld	l, a
	ld	a, (_snd_output_mode)
	or	a
	jr	nz, 3$
	; DIRECT: the latch byte, then period bits 9..4, to the held port
	ld	c, (hl)
	inc	l			; VD_REG_LATCH
	ld	a, e
	and	#0x0F
	or	(hl)
	out	(c), a
	ld	a, e
	and	#0xF0
	or	d
	rrca
	rrca
	rrca
	rrca
	out	(c), a
	ret
3$:
	; the latch byte: tone latch | period's low nibble
	inc	l			; VD_REG_LATCH
	ld	a, e
	and	#0x0F
	or	(hl)
	push	de
	call	_desc_emit
	pop	de
	; the data byte: period bits 9..4
	ld	a, e
	rrca
	rrca
	rrca
	rrca
	and	#0x0F
	ld	e, a
	ld	a, d
	rlca
	rlca
	rlca
	rlca
	and	#0xF0
	or	e
	jp	_desc_emit

; ---------------------------------------------------------------------------
; snd_put_noise: HL = descriptor base, A = control byte, B != 0 forces.
; ---------------------------------------------------------------------------
snd_put_noise::
	ld	c, a
	ld	a, l
	and	#0xF0
	or	#VD_DESIRED_REG
	ld	l, a
	ld	e, (hl)
	inc	l
	ld	d, (hl)			; DE = &desired noise
	ld	a, c
	ld	(de), a
	ld	a, (_snd_all_valid)
	or	a
	jr	nz, 1$
	call	_chip_invalid
	ret	z
1$:
	inc	l
	ld	e, (hl)
	inc	l
	ld	d, (hl)			; DE = &shadow noise
	ld	a, b
	or	a
	jr	nz, 2$
	ld	a, (de)
	cp	c
	ret	z
2$:
	ld	a, c
	ld	(de), a
	jp	_desc_emit

_snd_output_flush::
	ld	a, (_snd_dirty)
	or	a
	jp	z, _flush_batch
	xor	a
	ld	(_snd_dirty), a
	ld	a, (_snd_all_valid)
	or	a
	jp	z, _flush_all
	jp	_flush_batch

; A=value, current chip/port in the rewrite working bytes.
_rewrite_emit:
	ld	b, a
	ld	a, (_snd_output_mode)
	or	a
	jr	nz, 1$
	ld	a, (_snd_rw_port)
	ld	c, a
	out	(c), b
	ret
1$:
	ld	a, (_snd_batch_count)
	cp	#SND_BATCH_MAX
	jr	c, 2$
	push	bc
	call	snd_batch_send
	pop	bc
2$:
	ld	a, (_snd_batch_count)
	add	a, a
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_batch
	add	hl, de
	ld	a, (_snd_rw_chip)
	ld	(hl), a
	inc	hl
	ld	(hl), b
	ld	a, (_snd_batch_count)
	inc	a
	ld	(_snd_batch_count), a
	ret

; DIRECT: the shadow copies the desired periods and noise in one block
; (the arrays are contiguous), then each chip's eleven bytes go out in
; card order with its port held in C.
_flush_direct:
	ld	hl, #_snd_desired_period
	ld	de, #_snd_shadow_period
	ld	bc, #SND_CHIP_COUNT * 7
	ldir
	ld	hl, #_snd_desired_period
	xor	a
	ld	(_snd_rw_chip), a
_fd_chip:
	push	hl
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_ports
	add	hl, de
	ld	c, (hl)			; C = the chip's port
	pop	hl
	ld	b, #0x80		; the tone latch
_fd_tone:
	ld	e, (hl)
	inc	hl
	ld	d, (hl)			; DE = the period
	inc	hl
	ld	a, e
	and	#0x0F
	or	b
	out	(c), a
	ld	a, e
	and	#0xF0
	or	d
	rrca
	rrca
	rrca
	rrca				; period bits 9..4
	out	(c), a
	ld	a, b
	add	a, #0x20
	ld	b, a
	cp	#0xE0
	jr	nz, _fd_tone
	push	hl
	ld	a, (_snd_rw_chip)
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_desired_noise
	add	hl, de
	ld	a, (hl)
	out	(c), a
	; the four attenuations from the chip's descriptors
	ld	a, e
	rrca
	rrca				; chip * 64
	or	#VD_ATT_DESIRED
	ld	l, a
	ld	a, (_snd_desc_page)
	ld	h, a
	ld	b, #4
_fd_atten:
	ld	a, (hl)			; desired
	inc	l
	ld	(hl), a			; shadow
	inc	l
	or	(hl)			; | the latch
	out	(c), a
	ld	a, l
	add	a, #14
	ld	l, a
	djnz	_fd_atten
	pop	hl
	ld	a, (_snd_rw_chip)
	inc	a
	ld	(_snd_rw_chip), a
	cp	#SND_CHIP_COUNT
	jr	nz, _fd_chip
	jp	_flush_valid

; Complete 44-register card-order rewrite from desired state.
_flush_all:
	ld	a, (_snd_output_mode)
	or	a
	jr	z, _flush_direct
	xor	a
	ld	(_snd_rw_chip), a
_rw_chip_loop:
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_ports
	add	hl, de
	ld	a, (hl)
	ld	(_snd_rw_port), a
	; period desired/shadow bases: chip * 6
	ld	a, e
	add	a, a
	ld	c, a
	add	a, a
	add	a, c
	ld	c, a
	ld	b, #0
	ld	hl, #_snd_desired_period
	add	hl, bc
	ld	de, #_snd_shadow_period
	ex	de, hl
	add	hl, bc
	ex	de, hl
	ld	b, #3
	xor	a
	ld	(_snd_rw_latch), a
_rw_tone:
	push	bc
	ld	c, (hl)
	ld	a, c
	ld	(de), a
	inc	hl
	inc	de
	ld	b, (hl)
	ld	a, b
	ld	(de), a
	inc	hl
	inc	de
	push	hl
	push	de
	ld	a, (_snd_rw_latch)
	or	#0x80
	ld	e, a
	ld	a, c
	and	#0x0F
	or	e
	push	bc
	call	_rewrite_emit
	pop	bc
	ld	a, c
	rrca
	rrca
	rrca
	rrca
	and	#0x0F
	ld	e, a
	ld	a, b
	rlca
	rlca
	rlca
	rlca
	and	#0xF0
	or	e
	call	_rewrite_emit
	pop	de
	pop	hl
	ld	a, (_snd_rw_latch)
	add	a, #0x20
	ld	(_snd_rw_latch), a
	pop	bc
	djnz	_rw_tone
	; noise
	ld	a, (_snd_rw_chip)
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_desired_noise
	add	hl, de
	ld	c, (hl)
	ld	hl, #_snd_shadow_noise
	add	hl, de
	ld	(hl), c
	ld	a, c
	call	_rewrite_emit
	; the four attenuations from the chip's descriptors (voice chip*4+sub)
	ld	a, (_snd_rw_chip)
	rlca
	rlca
	rlca
	rlca
	rlca
	rlca			; chip * 64 = the chip's first descriptor
	or	#VD_ATT_DESIRED
	ld	l, a
	ld	a, (_snd_desc_page)
	ld	h, a
	ld	b, #4
_rw_atten:
	push	bc
	ld	c, (hl)			; desired
	inc	l
	ld	(hl), c			; shadow
	inc	l
	ld	a, (hl)			; latch
	or	c
	push	hl
	call	_rewrite_emit
	pop	hl
	ld	a, l
	add	a, #14			; the next descriptor's desired
	ld	l, a
	pop	bc
	djnz	_rw_atten
	ld	a, (_snd_rw_chip)
	inc	a
	ld	(_snd_rw_chip), a
	cp	#SND_CHIP_COUNT
	jp	nz, _rw_chip_loop
_flush_valid:
	ld	hl, #_snd_shadow_valid
	ld	b, #SND_CHIP_COUNT
	ld	a, #1
_rw_valid:
	ld	(hl), a
	inc	hl
	djnz	_rw_valid
	ld	(_snd_all_valid), a

_flush_batch:
snd_batch_send::
	ld	a, (_snd_batch_count)
	or	a
	ret	z
	ld	a, (_snd_output_mode)
	or	a
	ret	z
	ld	hl, (_snd_handle)
	ld	de, #_snd_batch
	ld	a, (_snd_batch_count)
	ld	c, a
	ld	b, #0
	push	bc
	call	_sound_write
	ld	a, (_snd_batch_count)
	ld	b, a
	ld	a, e
	cp	b
	jr	nz, _flush_failed
	ld	a, d
	or	a
	jr	nz, _flush_failed
	xor	a
	ld	(_snd_batch_count), a
	ret
_flush_failed:
	xor	a
	ld	(_snd_batch_count), a
	call	_snd_shadow_invalidate
	ret
