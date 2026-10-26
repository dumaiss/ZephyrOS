; -----------------------------------------------------------------------------
; libsnd manual voice-program engine.  Sixteen 16-byte descriptors fill one
; 256-byte-aligned page, walked in voice order with HL (H = the page, L =
; voice << 4 | field): no IX/IY, no index arithmetic.  The next page holds
; a copy of snd_level_table, so a level's attenuation is (H+1):level.
;
; Idle ticks cost one compare: the walk records the earliest wait among
; the active voices (the countdown) and the engine only accumulates the
; elapsed ticks until it is reached.  A walk then applies the whole
; accumulated count: a voice whose wait is longer only counts it down; a
; voice that becomes due runs its program with the remainder (always
; under one call's capped count), so the result equals stepping one tick
; at a time.  A program installed since the last walk runs with this
; call's count only.
;
; Per voice, the walk commits the period, the noise, then the
; attenuation (psg.c's order).  The attenuation is compared and written
; inline when the output is DIRECT and the shadow valid; everything else
; goes through the write-through primitives in snd_out.asm.
; -----------------------------------------------------------------------------

	.module snd_voice

	.globl _snd_engine
	.globl _snd_voice_init
	.globl _snd_voice_install
	.globl _snd_voice_clear
	.globl _snd_voice_level
	.globl _snd_desc_page
	.globl _snd_voice_active
	.globl _snd_programs
	.globl _snd_level_table
	.globl _snd_ports
	.globl _snd_desired_period
	.globl _snd_desired_noise
	.globl _snd_voice_work
	.globl _snd_due_work
	.globl _snd_shadow_period
	.globl _snd_shadow_noise
	.globl _snd_output_mode
	.globl _snd_all_valid
	.globl snd_put_period
	.globl snd_put_atten
	.globl snd_put_noise

SND_VOICE_COUNT   = 16
SND_PROGRAM_BYTES = 86
SND_P_END         = 0
SND_P_PERIOD      = 1
SND_P_ATTEN       = 2
SND_P_NOISE       = 3
SND_P_WAIT8       = 4
SND_P_WAIT16      = 5

; Descriptor layout (16 bytes; mirrored in snd_out.asm, snd_private.h).
VD_FLAGS         = 0		; VF_* below; 0 = nothing to do
VD_WAIT          = 1		; 16-bit
VD_CURSOR        = 3		; 16-bit
VD_LEVEL         = 5		; the current linear level
VD_ATT_DESIRED   = 6
VD_ATT_SHADOW    = 7
VD_ATT_LATCH     = 8
VD_PORT          = 9
VD_REG_LATCH     = 10
VD_CHIP          = 11
VD_DESIRED_REG   = 12		; pointer: the period pair or the noise byte
VD_SHADOW_REG    = 14		; pointer

VF_ACTIVE        = 0x01
VF_ATTEN_PENDING = 0x02		; a foreground silence to commit
VF_FORCE         = 0x04		; the noise voice: a forced control write

TOUCH_PERIOD     = 0x01
TOUCH_NOISE      = 0x04

	.area _DATA

_snd_desc_area:
	.ds 768			; the aligned descriptor and level pages
_snd_desc_page::
	.ds 1
_snd_elapsed::
	.ds 2
_snd_remaining::
	.ds 2
_snd_pending:
	.ds 2			; ticks accumulated since the last walk
_snd_now:
	.ds 2			; this call's ticks: a fresh program's budget
_snd_countdown:
	.ds 2			; the earliest active wait at the last walk
_snd_touched:
	.ds 1			; the stepping voice's period/noise changes
_snd_fast:
	.ds 1			; DIRECT and a valid shadow, for this walk
_snd_voice_work::
	.ds 1
_snd_work_seen::
	.ds 1
_snd_due_work::
	.ds 1

	.area _CODE

; ---------------------------------------------------------------------------
; snd_voice_init: the descriptor page, the level page and every
; descriptor's invariant half, after the ports are loaded.  Foreground.
; ---------------------------------------------------------------------------
_snd_voice_init::
	ld	hl, #_snd_desc_area + 255
	ld	a, h
	ld	(_snd_desc_page), a
	; the level table's copy on the next page
	inc	a
	ld	d, a
	ld	e, #0
	ld	hl, #_snd_level_table
	ld	bc, #256
	ldir
	xor	a
	ld	(_snd_voice_work), a
	ld	(_snd_due_work), a
	ld	hl, #0
	ld	(_snd_pending), hl
	dec	hl
	ld	(_snd_countdown), hl
	ld	a, (_snd_desc_page)
	ld	h, a
	ld	l, #0
	ld	c, #0			; C = the voice
_vi_loop:
	ld	(hl), #0		; VD_FLAGS
	inc	l
	ld	(hl), #0xFF		; VD_WAIT
	inc	l
	ld	(hl), #0xFF
	inc	l
	ld	(hl), #0		; VD_CURSOR
	inc	l
	ld	(hl), #0
	inc	l
	ld	(hl), #0		; VD_LEVEL
	inc	l
	ld	(hl), #0x0F		; VD_ATT_DESIRED
	inc	l
	ld	(hl), #0x0F		; VD_ATT_SHADOW
	inc	l
	ld	a, c
	and	#3
	rrca
	rrca
	rrca
	ld	b, a			; B = sub << 5
	or	#0x90
	ld	(hl), a			; VD_ATT_LATCH
	inc	l
	push	hl
	ld	a, c
	rrca
	rrca
	and	#0x03
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_ports
	add	hl, de
	ld	a, (hl)
	pop	hl
	ld	(hl), a			; VD_PORT
	inc	l
	ld	a, b
	or	#0x80
	ld	(hl), a			; VD_REG_LATCH (E0h for the noise voice)
	inc	l
	ld	(hl), e			; VD_CHIP
	inc	l
	; the register pointers: the period pair, or the chip's noise byte
	push	hl
	ld	a, c
	and	#3
	cp	#3
	jr	z, _vi_noise
	add	a, a			; sub * 2
	ld	b, a
	ld	a, e
	add	a, a			; chip * 2
	ld	d, a
	add	a, a			; chip * 4
	add	a, d			; chip * 6
	add	a, b
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_desired_period
	add	hl, de
	push	hl
	ld	hl, #_snd_shadow_period
	add	hl, de
	jr	_vi_store
_vi_noise:
	ld	d, #0
	ld	hl, #_snd_desired_noise
	add	hl, de
	push	hl
	ld	hl, #_snd_shadow_noise
	add	hl, de
_vi_store:
	ex	de, hl			; DE = &shadow
	pop	bc			; BC = &desired
	pop	hl
	ld	(hl), c			; VD_DESIRED_REG
	inc	l
	ld	(hl), b
	inc	l
	ld	(hl), e			; VD_SHADOW_REG
	inc	l
	ld	(hl), d
	inc	l			; the next descriptor
	ld	a, l
	rrca
	rrca
	rrca
	rrca
	and	#0x0F
	ld	c, a			; the next voice
	ld	a, l
	or	a
	jp	nz, _vi_loop
	ret

; ---------------------------------------------------------------------------
; snd_engine: HL = elapsed ticks (already capped by snd_tick).
; ---------------------------------------------------------------------------
_snd_engine::
	ld	a, (_snd_voice_work)
	or	a
	ret	z
	ld	(_snd_now), hl
	ld	de, (_snd_pending)
	add	hl, de
	ld	a, (_snd_due_work)
	or	a
	jr	nz, _engine_walk
	; nothing due before the countdown: accumulate
	ld	(_snd_pending), hl
	ld	de, (_snd_countdown)
	or	a
	sbc	hl, de
	ret	c
	ld	hl, (_snd_pending)
_engine_walk:
	ld	(_snd_elapsed), hl
	ld	hl, #0
	ld	(_snd_pending), hl
	dec	hl
	ld	(_snd_countdown), hl
	xor	a
	ld	(_snd_due_work), a
	ld	(_snd_work_seen), a
	ld	a, (_snd_output_mode)
	or	a
	ld	a, #0
	jr	nz, 1$
	ld	a, (_snd_all_valid)
1$:
	ld	(_snd_fast), a
	ld	a, (_snd_desc_page)
	ld	h, a
	ld	l, #0
	jr	_voice_loop

_voice_skip:				; L = the descriptor base
	ld	a, l
	add	a, #16
	ld	l, a
	jr	z, _walk_done
_voice_loop:
	ld	a, (hl)
	or	a
	jr	z, _voice_skip
	cp	#VF_ACTIVE
	jr	nz, _voice_slow
_voice_active:				; L = the descriptor base
	ld	a, #1
	ld	(_snd_work_seen), a
	inc	l			; VD_WAIT
	ld	c, (hl)
	inc	l
	ld	b, (hl)			; BC = the wait, L = VD_WAIT + 1
	ld	a, b
	or	c
	jr	z, _voice_fresh
	push	hl
	ld	hl, (_snd_elapsed)
	or	a
	sbc	hl, bc			; elapsed - wait
	jr	c, _voice_waiting
	ld	(_snd_remaining), hl
	pop	hl
	jr	_ops
_voice_fresh:				; installed since the last walk
	ld	bc, (_snd_now)
	ld	(_snd_remaining), bc
	jr	_ops
_voice_waiting:				; HL = elapsed - wait < 0: wait = -HL
	xor	a
	sub	l
	ld	c, a
	sbc	a, a
	sub	h
	ld	b, a
	pop	hl
	ld	(hl), b
	dec	l
	ld	(hl), c
	call	_track_min
	jp	_voice_next

_walk_done:
	ld	a, (_snd_work_seen)
	ld	(_snd_voice_work), a
	ret

; The pending silence, the forced noise write, then the program.
_voice_slow:				; A = the flags, L = the base
	bit	1, a
	jr	z, 1$
	res	1, (hl)
	push	hl
	ld	a, l
	or	#VD_ATT_DESIRED
	ld	l, a
	ld	a, (hl)
	pop	hl
	push	hl
	call	snd_put_atten
	pop	hl
1$:
	bit	2, (hl)
	jr	z, 2$
	res	2, (hl)
	push	hl
	ld	a, l
	or	#VD_DESIRED_REG
	ld	l, a
	ld	e, (hl)
	inc	l
	ld	d, (hl)
	ld	a, (de)			; the desired noise byte
	pop	hl
	push	hl
	ld	b, #1
	call	snd_put_noise
	pop	hl
2$:
	bit	0, (hl)
	jr	nz, _voice_active
	jp	_voice_next

; The program: HL = the descriptor at VD_LEVEL, DE = the cursor.
_ops:					; L = VD_WAIT + 1
	inc	l
	ld	e, (hl)
	inc	l
	ld	d, (hl)
	inc	l			; VD_LEVEL
	xor	a
	ld	(_snd_touched), a
_op_loop:
	ld	a, (de)
_op_dispatch:
	cp	#SND_P_ATTEN
	jr	z, _op_atten
	cp	#SND_P_WAIT8
	jr	z, _op_wait8
	cp	#SND_P_PERIOD
	jp	z, _op_period
	cp	#SND_P_WAIT16
	jp	z, _op_wait16
	cp	#SND_P_NOISE
	jp	z, _op_noise
	jp	_op_end			; END, or a corrupt byte: stop safely

; ATTEN level: the level, and its attenuation from the level page.
_op_atten:
	inc	de
	ld	a, (de)
	inc	de
	ld	(hl), a			; VD_LEVEL
	ld	c, a
	ld	b, h
	inc	b
	ld	a, (bc)
	inc	l
	ld	(hl), a			; VD_ATT_DESIRED
	dec	l
	ld	a, (de)
	cp	#SND_P_WAIT8
	jr	nz, _op_dispatch

_op_wait8:
	inc	de
	ld	a, (de)
	inc	de
	ld	c, a
	ld	b, #0
_op_wait:				; BC = n, DE = the cursor past it
	push	hl
	ld	hl, (_snd_remaining)
	ld	a, h
	or	l
	jr	z, _wait_store		; due exactly: wait n
	or	a
	sbc	hl, bc			; remaining - n
	jr	c, _wait_residual
	jr	z, _wait_next_tick
	ld	(_snd_remaining), hl	; catch-up: the next logical tick
	pop	hl
	jp	_op_loop
_wait_next_tick:
	ld	bc, #1
	jr	_wait_store
_wait_residual:				; HL = remaining - n < 0: wait = -HL
	xor	a
	sub	l
	ld	c, a
	sbc	a, a
	sub	h
	ld	b, a
_wait_store:
	pop	hl
	ld	a, l
	sub	#VD_LEVEL - VD_WAIT
	ld	l, a
	ld	(hl), c
	inc	l
	ld	(hl), b
	inc	l			; VD_CURSOR
	ld	(hl), e
	inc	l
	ld	(hl), d
	push	hl			; the countdown: the earliest wait
	ld	hl, (_snd_countdown)
	or	a
	sbc	hl, bc
	jr	c, 1$
	ld	(_snd_countdown), bc
1$:
	pop	hl
	jr	_commit

_op_wait16:
	inc	de
	ld	a, (de)
	ld	c, a
	inc	de
	ld	a, (de)
	ld	b, a
	inc	de
	jr	_op_wait

_op_period:
	inc	de
	ld	a, (de)
	ld	c, a
	inc	de
	ld	a, (de)
	ld	b, a			; BC = the period
	inc	de
	push	de
	ld	a, l
	and	#0xF0
	or	#VD_DESIRED_REG
	ld	l, a
	ld	e, (hl)
	inc	l
	ld	d, (hl)
	ld	a, c
	ld	(de), a
	inc	de
	ld	a, b
	ld	(de), a
	pop	de
	ld	a, (_snd_touched)
	or	#TOUCH_PERIOD
	jr	_op_reg_done

_op_noise:
	inc	de
	ld	a, (de)
	inc	de
	or	#0xE0
	ld	c, a
	push	de
	ld	a, l
	and	#0xF0
	or	#VD_DESIRED_REG
	ld	l, a
	ld	e, (hl)
	inc	l
	ld	d, (hl)
	ld	a, c
	ld	(de), a
	pop	de
	ld	a, (_snd_touched)
	or	#TOUCH_NOISE
_op_reg_done:
	ld	(_snd_touched), a
	ld	a, l
	and	#0xF0
	or	#VD_LEVEL
	ld	l, a
	jp	_op_loop

_op_end:
	ld	a, l
	and	#0xF0
	ld	l, a
	res	0, (hl)			; inactive
	inc	l
	ld	(hl), #0xFF		; wait: never
	inc	l
	ld	(hl), #0xFF
	push	hl
	ld	a, l
	rrca
	rrca
	rrca
	rrca
	and	#0x0F
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_voice_active
	add	hl, de
	ld	(hl), #0
	pop	hl
	inc	l
	inc	l			; VD_CURSOR + 1

; Commit this voice's changes: the period, the noise, then the
; attenuation.  L = VD_CURSOR + 1.
_commit:
	ld	a, (_snd_touched)
	or	a
	jr	nz, _commit_touched
_commit_atten:				; L = VD_CURSOR + 1
	inc	l
	inc	l			; VD_ATT_DESIRED
	ld	a, (_snd_fast)
	or	a
	jr	z, _commit_atten_slow
	ld	a, (hl)			; desired
	inc	l
	cp	(hl)			; shadow
	jr	z, _voice_next
	ld	(hl), a
	inc	l
	or	(hl)			; | the latch
	inc	l
	ld	c, (hl)			; the port
	out	(c), a
	jr	_voice_next
_commit_atten_slow:
	ld	a, (hl)
	push	hl
	call	snd_put_atten
	pop	hl

_voice_next:
	ld	a, l
	and	#0xF0
	add	a, #16
	ld	l, a
	jp	nz, _voice_loop
	jp	_walk_done

_commit_touched:
	call	_commit_regs
	jr	_commit_atten

; The touched period and noise: A = the touch bits.  Keeps HL.
_commit_regs:
	push	hl
	ld	c, a
	bit	0, c
	jr	z, 1$
	push	bc
	ld	a, l
	and	#0xF0
	or	#VD_DESIRED_REG
	ld	l, a
	ld	e, (hl)
	inc	l
	ld	d, (hl)
	ex	de, hl
	ld	e, (hl)
	inc	hl
	ld	d, (hl)			; DE = the desired period
	pop	bc
	pop	hl
	push	hl
	push	bc
	call	snd_put_period
	pop	bc
	pop	hl
	push	hl
1$:
	bit	2, c
	jr	z, 2$
	ld	a, l
	and	#0xF0
	or	#VD_DESIRED_REG
	ld	l, a
	ld	e, (hl)
	inc	l
	ld	d, (hl)
	ld	a, (de)
	pop	hl
	push	hl
	ld	b, #1
	call	snd_put_noise
2$:
	pop	hl
	ret

; The countdown keeps the earliest active wait: BC = a voice's wait.
; Keeps HL, BC, DE.
_track_min:
	push	hl
	ld	hl, (_snd_countdown)
	or	a
	sbc	hl, bc			; countdown - wait
	pop	hl
	ret	c
	ld	(_snd_countdown), bc
	ret

; A = voice.  HL = its descriptor base.
_descriptor_for_a:
	rlca
	rlca
	rlca
	rlca
	and	#0xF0
	ld	l, a
	ld	a, (_snd_desc_page)
	ld	h, a
	ret

; uint8_t snd_voice_level(uint8_t voice): A = voice -> A = its level.
_snd_voice_level::
	call	_descriptor_for_a
	ld	a, l
	or	#VD_LEVEL
	ld	l, a
	ld	a, (hl)
	ret

; A = voice.  Publish a freshly compiled program atomically.
_snd_voice_install::
	di
	ld	c, a			; C = the voice
	call	_descriptor_for_a
	push	hl
	; cursor = programs + voice * 86
	ld	l, c
	ld	h, #0
	add	hl, hl			; * 2
	ld	d, h
	ld	e, l
	add	hl, hl			; * 4
	push	hl
	add	hl, hl
	add	hl, hl			; * 16
	push	hl
	add	hl, hl
	add	hl, hl			; * 64
	pop	de			; * 16
	add	hl, de			; * 80
	pop	de			; * 4
	add	hl, de			; * 84
	ld	e, c
	ld	d, #0
	add	hl, de
	add	hl, de			; * 86
	ld	de, #_snd_programs
	add	hl, de
	ex	de, hl			; DE = the program
	pop	hl			; the descriptor
	ld	a, (hl)
	and	#VF_FORCE
	or	#VF_ACTIVE
	ld	(hl), a
	inc	l
	ld	(hl), #0		; wait 0: runs this tick
	inc	l
	ld	(hl), #0
	inc	l
	ld	(hl), e			; the cursor
	inc	l
	ld	(hl), d
	inc	l
	ld	(hl), #0		; VD_LEVEL
	ld	a, #1
	ld	(_snd_voice_work), a
	ld	(_snd_due_work), a
	ld	e, c
	ld	d, #0
	ld	hl, #_snd_voice_active
	add	hl, de
	ld	(hl), #1
	ei
	ret

; A = voice.  Idle it and publish a pending attenuation-15 write.
_snd_voice_clear::
	di
	ld	c, a
	call	_descriptor_for_a
	ld	a, (hl)
	and	#VF_FORCE
	or	#VF_ATTEN_PENDING
	ld	(hl), a
	inc	l
	ld	(hl), #0xFF
	inc	l
	ld	(hl), #0xFF
	inc	l
	inc	l
	inc	l
	ld	(hl), #0		; VD_LEVEL
	inc	l
	ld	(hl), #0x0F		; VD_ATT_DESIRED
	ld	a, #1
	ld	(_snd_voice_work), a
	ld	(_snd_due_work), a
	ld	e, c
	ld	d, #0
	ld	hl, #_snd_voice_active
	add	hl, de
	ld	(hl), #0
	ei
	ret
