; -----------------------------------------------------------------------------
; libsnd output engine (user/sdk/snd/src): the per-chip desired state, the
; hardware shadow and the flush to the ports.
;
; The C setup (snd_open) mirrors the card's ports into the bytes below;
; every musical source writes the *desired* state (three tone periods,
; four attenuations and the noise control per chip) and queues the
; changed slot (0-7) into the chip's slot queue.  This module turns the
; queue into register writes in the card's order: per chip, the tone
; latch+data pairs 0-2, then the noise control (forced when the force
; flag is set, so a phase reset writes even when the value matches),
; then the four attenuations.
;
;   - DIRECT: OUT (C),A to the port sound_info reported;
;   - BATCH: the writes accumulate in a static array and one sound_write
;     sends them at the end of the tick.
;
; Both modes produce the same trace.  An invalidated shadow (open, or a
; failed batch) makes the flush rewrite everything from the desired
; state.  A failed sound_write marks the shadow uncertain and mutes;
; the next flush rewrites everything.
;
; Call convention (sdcccall(1)):
;   void snd_load_ports(const struct ksound_info *info)   HL = info
;   void snd_shadow_invalidate(void)
;   void snd_output_flush(void)
;   void snd_noise_apply(uint8_t noise_voice, uint8_t control)
;                                                         A = voice, L = ctrl
;   void snd_queue_push(uint8_t chip, uint8_t slot)       A = chip, L = slot
; -----------------------------------------------------------------------------

	.module snd_out

	.globl _snd_load_ports
	.globl _snd_shadow_invalidate
	.globl _snd_output_flush
	.globl _snd_noise_apply
	.globl _snd_queue_push
	.globl snd_batch_send
	.globl _sound_write
	.globl _snd_handle
	.globl _snd_output_mode
	.globl _snd_dirty

SND_CHIP_COUNT     = 4
SND_BATCH_MAX      = 60
; 60 pairs: a full rewrite (44) plus a ZVG tick's appended run (<= 16)
SND_P_NOISE_BYTE   = 0xE0
SND_QUEUE_STRIDE   = 16

	.area _DATA

_snd_ports::
	.ds SND_CHIP_COUNT
_snd_desired_period::
	.ds SND_CHIP_COUNT * 3 * 2
_snd_desired_atten::
	.ds SND_CHIP_COUNT * 4
_snd_desired_noise::
	.ds SND_CHIP_COUNT
_snd_noise_force::
	.ds SND_CHIP_COUNT
_snd_shadow_period::
	.ds SND_CHIP_COUNT * 3 * 2
_snd_shadow_atten::
	.ds SND_CHIP_COUNT * 4
_snd_shadow_noise::
	.ds SND_CHIP_COUNT
_snd_shadow_valid::
	.ds SND_CHIP_COUNT
_snd_batch::
	.ds SND_BATCH_MAX * 2
_snd_batch_count::
	.ds 1
; The changed-slot queues: 16 bytes per chip (index = chip*16 | count),
; the entries are the slots 0-7 (tone 0-2, noise 3, atten 4-7).
_snd_slot_queue::
	.ds SND_CHIP_COUNT * SND_QUEUE_STRIDE
_snd_slot_count::
	.ds SND_CHIP_COUNT
; One bit per chip with queued slots: the flush skips empty chips.
_snd_chip_mask::
	.ds 1
; The flush's working copy of the mask, consumed chip by chip.
_snd_mask_work::
	.ds 1
; The flush's working state.
_snd_flush_chip::
	.ds 1
; The current chip's DAC base port, cached for the direct emitters.
_snd_port::
	.ds 1
_snd_tmp::
	.ds 1
_snd_tmp2::
	.ds 2
_snd_tmp3::
	.ds 2
_snd_dirty::
	.ds 1

	.area _CODE

; ---------------------------------------------------------------------------
; void snd_load_ports(const struct ksound_info *info);  HL = info
;   Mirror the four PSG ports into the fixed bytes (psg_ports is at
;   offset 4 in struct ksound_info).
; ---------------------------------------------------------------------------
_snd_load_ports::
	ld	de, #_snd_ports
	ld	b, #SND_CHIP_COUNT
	ld	c, #0x04
1$:
	push	bc
	push	hl
	ld	a, l
	add	a, c
	ld	l, a
	jp	nc, 2$
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

; ---------------------------------------------------------------------------
; void snd_shadow_invalidate(void);
;   Unknown hardware state and a muted desired state: the next flush
;   rewrites everything, and everything silent.
; ---------------------------------------------------------------------------
_snd_shadow_invalidate::
	ld	hl, #_snd_shadow_valid
	ld	b, #SND_CHIP_COUNT
	xor	a
1$:
	ld	(hl), a
	inc	hl
	djnz	1$
	ld	hl, #_snd_desired_atten
	ld	b, #SND_CHIP_COUNT * 4
	ld	a, #0x0F
2$:
	ld	(hl), a
	inc	hl
	djnz	2$
	ld	hl, #_snd_desired_noise
	ld	b, #SND_CHIP_COUNT
	ld	a, #SND_P_NOISE_BYTE
3$:
	ld	(hl), a
	inc	hl
	djnz	3$
	ld	hl, #_snd_noise_force
	ld	b, #SND_CHIP_COUNT
	xor	a
4$:
	ld	(hl), a
	inc	hl
	djnz	4$
	ld	a, #1
	ld	(_snd_dirty), a
	ret

; ---------------------------------------------------------------------------
; void snd_noise_apply(uint8_t noise_voice, uint8_t control);  A, L
;   The desired noise byte for the chip (E0h | control) with the forced
;   write flag, so a rate/mode change is always written.
; ---------------------------------------------------------------------------
_snd_noise_apply::
	ld	(_snd_tmp), a		; the voice
	ld	a, l
	or	#SND_P_NOISE_BYTE
	ld	(_snd_tmp2), a		; the desired byte
	ld	a, (_snd_tmp)
	srl	a
	srl	a			; chip = voice >> 2
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_desired_noise
	add	hl, de
	ld	a, (_snd_tmp2)
	ld	(hl), a
	ld	hl, #_snd_noise_force
	add	hl, de
	ld	(hl), #1
	ld	l, #3			; the noise slot
	ld	a, e			; the chip
	call	_snd_queue_push
	ld	a, #1
	ld	(_snd_dirty), a
	ret

; ---------------------------------------------------------------------------
; void snd_queue_push(uint8_t chip, uint8_t slot);  A = chip, L = slot
;   Append the changed slot to the chip's queue (16 slots per chip, one
;   tick's writers never exceed the stride).
; ---------------------------------------------------------------------------
_snd_queue_push::		; A = chip, L = slot
	ld	c, l			; C = the slot
	ld	e, a
	ld	d, #0			; DE = chip
	ld	hl, #_snd_slot_count
	add	hl, de
	ld	b, (hl)		; B = the old count
	inc	(hl)			; count++
	ld	hl, #_snd_chip_bits
	add	hl, de
	ld	a, (_snd_chip_mask)
	or	(hl)			; mask |= 1 << chip
	ld	(_snd_chip_mask), a
	ld	a, e
	add	a, a
	add	a, a
	add	a, a
	add	a, a			; chip * 16
	or	b			; the queue index
	ld	e, a
	ld	hl, #_snd_slot_queue
	add	hl, de
	ld	(hl), c
	ret

_snd_chip_bits:
	.db	0x01, 0x02, 0x04, 0x08

; ---------------------------------------------------------------------------
; void snd_output_flush(void);
;   A no-change tick leaves the dirty flag clear and returns in a dozen
;   cycles.  Otherwise: an invalidated shadow rewrites everything; a
;   clean shadow writes only the queued slots, chip by chip; the batch
;   (BATCH mode) goes out last.
; ---------------------------------------------------------------------------
_snd_output_flush::
	ld	a, (_snd_dirty)
	or	a
	jr	nz, _flush_go
	ld	a, (_snd_batch_count)
	or	a
	ret	z			; nothing dirty and no batch to send
_flush_go:
	xor	a
	ld	(_snd_dirty), a
	; the batch count is kept: a ZVG tick appends before this flush
	; and the pre-existing pairs ride the same sound_write (the
	; success path below clears it)
	ld	hl, #_snd_shadow_valid
	ld	b, #SND_CHIP_COUNT
1$:
	ld	a, (hl)
	or	a
	jp	z, _flush_all
	inc	hl
	djnz	1$
	xor	a
	ld	(_snd_flush_chip), a
	ld	a, (_snd_chip_mask)
	ld	(_snd_mask_work), a
	xor	a
	ld	(_snd_chip_mask), a
_chip_loop:
	ld	a, (_snd_mask_work)
	or	a
	jp	z, _flush_batch
	rra			; this chip's bit into the carry
	ld	(_snd_mask_work), a
	jr	nc, _chip_next
	ld	a, (_snd_flush_chip)
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_slot_count
	add	hl, de
	ld	b, (hl)		; B = the count
	xor	a
	ld	(hl), a		; consumed
	ld	a, (_snd_flush_chip)
	add	a, a
	add	a, a
	add	a, a
	add	a, a
	ld	l, a
	ld	h, #0
	ld	de, #_snd_slot_queue
	add	hl, de
	; cache the chip's port for the direct emitters
	ld	a, (_snd_flush_chip)
	ld	e, a
	ld	d, #0
	push	hl
	ld	hl, #_snd_ports
	add	hl, de
	ld	a, (hl)
	ld	(_snd_port), a
	pop	hl
_queue_entry:
	ld	e, (hl)		; the slot
	inc	hl
	push	hl
	push	bc
	call	_slot_flush
	pop	bc
	pop	hl
	djnz	_queue_entry
_chip_next:
	ld	a, (_snd_flush_chip)
	inc	a
	ld	(_snd_flush_chip), a
	jp	_chip_loop

; One queued slot: E = slot, _snd_flush_chip = chip.
_slot_flush:
	ld	a, (_snd_flush_chip)
	ld	b, a			; B = chip
	ld	a, e
	ld	(_snd_tmp), a		; the slot
	cp	#3
	jp	nc, _slot_high
	; ---- a tone: chip*6 + slot*2 into the period pair ----
	ld	a, b
	add	a, a
	ld	e, a			; chip * 2
	add	a, a
	add	a, e			; chip * 6
	ld	e, a
	ld	a, (_snd_tmp)
	add	a, a
	add	a, e			; chip*6 + slot*2
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_desired_period
	add	hl, de
	ld	c, (hl)		; plo
	inc	hl
	ld	b, (hl)		; phi
	push	bc			; the period
	ld	de, #(_snd_shadow_period - _snd_desired_period)
	add	hl, de			; &shadow hi
	ld	a, (hl)
	dec	hl
	cp	b
	jr	nz, _tone_write
	ld	a, (hl)
	cp	c
	jr	nz, _tone_write
	pop	bc
	ret
_tone_write:
	ld	(hl), c			; shadow lo
	inc	hl
	ld	(hl), b			; shadow hi
	ld	a, c
	ld	(_snd_tmp2), a		; the period rides the temps
	ld	a, b
	ld	(_snd_tmp3), a
	ld	a, (_snd_tmp)
	rrca
	rrca
	rrca
	and	#0xE0
	or	#0x80
	ld	e, a
	ld	a, c
	and	#0x0F
	or	e
	call	_emit
	ld	a, (_snd_tmp2)
	rrca
	rrca
	rrca
	rrca
	and	#0x0F
	ld	e, a
	ld	a, (_snd_tmp3)
	add	a, a
	add	a, a
	add	a, a
	add	a, a
	and	#0xF0
	or	e
	call	_emit
	pop	bc
	ret

_slot_high:
	ld	a, (_snd_tmp)
	cp	#3
	jr	nz, _slot_atten
	; ---- the noise control ----
	ld	a, (_snd_flush_chip)
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_desired_noise
	add	hl, de
	ld	c, (hl)		; C = the desired byte
	ld	hl, #_snd_noise_force
	add	hl, de
	ld	a, (hl)
	or	a
	jr	nz, _noise_write
	ld	hl, #_snd_shadow_noise
	add	hl, de
	ld	a, (hl)
	cp	c
	ret	z
_noise_write:
	ld	hl, #_snd_shadow_noise
	add	hl, de
	ld	(hl), c
	ld	hl, #_snd_noise_force
	add	hl, de
	ld	(hl), #0
	ld	a, c
	jp	_emit

_slot_atten:
	ld	a, (_snd_flush_chip)
	add	a, a
	add	a, a			; chip * 4
	ld	e, a
	ld	a, (_snd_tmp)
	sub	#4			; the attenuation sub-voice
	add	a, e			; chip*4 + sub
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_desired_atten
	add	hl, de
	ld	c, (hl)		; C = the desired atten
	ld	de, #(_snd_shadow_atten - _snd_desired_atten)
	add	hl, de
	ld	a, (hl)
	cp	c
	ret	z
	ld	(hl), c
	ld	a, (_snd_tmp)
	sub	#4
	rrca
	rrca
	rrca
	and	#0xE0
	or	#0x90
	or	c
	jp	_emit

; The full rewrite after an invalidation: every register, in the card's
; order, with no compare.
_flush_all:
	; a full rewrite consumes every queued slot
	xor	a
	ld	hl, #_snd_slot_count
	ld	(hl), a
	inc	hl
	ld	(hl), a
	inc	hl
	ld	(hl), a
	inc	hl
	ld	(hl), a
	ld	(_snd_chip_mask), a
	ld	hl, #_snd_desired_period
	ld	de, #_snd_shadow_period
	xor	a
	ld	(_snd_flush_chip), a
_fa_chip:
	; cache the chip's port for the emitters
	push	hl
	push	de			; the shadow pointer
	ld	a, (_snd_flush_chip)
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_ports
	add	hl, de
	ld	a, (hl)
	ld	(_snd_port), a
	pop	de
	pop	hl
	ld	b, #3			; the tone counter (3, 2, 1)
_fa_tone:
	push	bc
	ld	a, #3
	sub	b			; the sub-voice 0-2
	ld	(_snd_tmp), a
	ld	c, (hl)		; plo
	inc	hl
	ld	b, (hl)		; phi
	ex	de, hl
	ld	(hl), c
	inc	hl
	ld	(hl), b
	dec	hl
	ex	de, hl
	inc	hl			; past the period pair
	push	hl
	push	de
	ld	a, c
	ld	(_snd_tmp2), a		; the period rides the temps
	ld	a, b
	ld	(_snd_tmp3), a
	ld	a, (_snd_tmp)
	rrca
	rrca
	rrca
	and	#0xE0
	or	#0x80
	ld	e, a
	ld	a, c
	and	#0x0F
	or	e
	call	_emit
	ld	a, (_snd_tmp2)
	rrca
	rrca
	rrca
	rrca
	and	#0x0F
	ld	e, a
	ld	a, (_snd_tmp3)
	add	a, a
	add	a, a
	add	a, a
	add	a, a
	and	#0xF0
	or	e
	call	_emit
	pop	de
	pop	hl
	inc	de
	inc	de
	pop	bc
	djnz	_fa_tone
	; the noise/atten helpers clobber HL/DE: save the tone pointers
	push	hl
	push	de
	call	_fa_noise
	call	_fa_attens
	pop	de
	pop	hl
	ld	a, (_snd_flush_chip)
	inc	a
	ld	(_snd_flush_chip), a
	cp	#SND_CHIP_COUNT
	jr	nz, _fa_chip
	ld	hl, #_snd_shadow_valid
	ld	b, #SND_CHIP_COUNT
	ld	a, #1
_fa_valid:
	ld	(hl), a
	inc	hl
	djnz	_fa_valid
	jp	_flush_batch

_fa_noise:
	ld	a, (_snd_flush_chip)
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_desired_noise
	add	hl, de
	ld	c, (hl)
	ld	hl, #_snd_shadow_noise
	add	hl, de
	ld	(hl), c
	ld	hl, #_snd_noise_force
	add	hl, de
	ld	(hl), #0
	ld	a, c
	jp	_emit

_fa_attens:
	ld	a, (_snd_flush_chip)
	add	a, a
	add	a, a			; chip * 4
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_desired_atten
	add	hl, de
	ld	(_snd_tmp2), hl		; the desired base
	ld	de, #(_snd_shadow_atten - _snd_desired_atten)
	add	hl, de
	ld	(_snd_tmp3), hl		; the shadow base
	ld	b, #4
	xor	a
	ld	(_snd_tmp), a		; the sub-voice
_fa_atten:
	ld	a, (_snd_tmp)
	ld	e, a
	ld	d, #0
	push	bc
	push	de
	ld	hl, (_snd_tmp2)
	add	hl, de
	ld	c, (hl)		; the desired atten
	ld	hl, (_snd_tmp3)
	add	hl, de
	ld	(hl), c			; the shadow
	pop	de
	ld	a, e
	rrca
	rrca
	rrca
	and	#0xE0
	or	#0x90
	or	c
	call	_emit
	pop	bc
	ld	a, (_snd_tmp)
	inc	a
	ld	(_snd_tmp), a
	djnz	_fa_atten
	ret

; One byte out: A = value.  DIRECT: OUT (C),A with the chip's port
; cached in _snd_port by the walk.  BATCH: append {chip, value} to the
; static array.  Preserves HL and DE; the callers keep their own
; registers.
_emit:
	ld	b, a			; the value
	ld	a, (_snd_output_mode)
	or	a
	jr	nz, _emit_batch
	ld	a, (_snd_port)
	ld	c, a
	ld	a, b
	out	(c), a
	ret
_emit_batch:
	ld	a, (_snd_batch_count)
	add	a, a
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_batch
	add	hl, de
	ld	a, (_snd_flush_chip)
	ld	(hl), a
	inc	hl
	ld	a, b
	ld	(hl), a
	ld	a, (_snd_batch_count)
	inc	a
	ld	(_snd_batch_count), a
	ret

; The tick's batch: one sound_write (BATCH mode only; DIRECT never
; queued).  A short or failed write marks the shadow uncertain and
; mutes, so the next flush rewrites everything.
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
	ld	c, a			; the count's low byte (PUSH stores B high)
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
	ld	a, #1
	ld	(_snd_dirty), a
	ret
