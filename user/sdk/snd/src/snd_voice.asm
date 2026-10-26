; -----------------------------------------------------------------------------
; libsnd voice-program engine (user/sdk/snd/src): the assembly half of the
; tick.  snd_engine steps the active voice programs by the elapsed ticks
; and writes the desired chip state; the program format lives in
; snd_private.h.  No C is called here.
;
;   void snd_engine(uint16_t elapsed);            HL = ticks
;   void snd_voice_install(uint8_t voice);        A = voice
;   void snd_voice_clear(uint8_t voice);          A = voice
;
; Scheduling.  The voices that can act this tick form the run list
; (head/next/prev, kept inside the per-voice state slots).  A WAIT parks
; its voice:
;
;   - n > remaining:  the voice leaves the run list and joins the wait
;     list with an absolute due time (clock + residual); the engine
;     only walks the wait list when the earliest due time arrives, so a
;     tick with nothing due costs one byte compare (the idle budget);
;   - n <= remaining: the residual is consumed now and the voice stays
;     on the run list with its pause flag set: the next tick resumes it
;     in place.  A WAIT therefore always ends the voice's current tick,
;     which is what makes one envelope step advance per tick.
;
; The desired-state writers queue the changed slot for the flush
; (_snd_queue_push) so the output
; engine emits in the card's register order.
;
; install/clear run with interrupts off: the cursor/wait publication
; and the list links are the one atomic index the tick and the C
; compiler share.
; -----------------------------------------------------------------------------

	.module snd_voice

	.globl _snd_engine
	.globl _snd_voice_install
	.globl _snd_voice_clear
	.globl _snd_voice_active
	.globl _snd_voice_level
	.globl _snd_voice_head
	.globl _snd_wait_head
	.globl _snd_min_due
	.globl _snd_programs
	.globl _snd_level_table
	.globl _snd_desired_period
	.globl _snd_desired_atten
	.globl _snd_desired_noise
	.globl _snd_noise_force
	.globl _snd_dirty
	.globl _snd_queue_push

SND_VOICE_COUNT   = 16
SND_PROGRAM_BYTES = 86
SND_P_END         = 0x00
SND_P_PERIOD      = 0x01
SND_P_ATTEN       = 0x02
SND_P_NOISE       = 0x03
SND_P_WAIT8       = 0x04
SND_P_WAIT16      = 0x05
SND_VOICE_NONE    = 0xFF
SND_F_PAUSED      = 0x01

	.area _DATA

; The per-voice engine state: one 8-byte slot per voice, so the walk
; loads the whole voice with one multiply and sequential reads:
;   +0 next (run list)   +1 prev (run list)
;   +2 wlo  +3 whi       +4 clo  +5 chi       +6 flags   +7 spare
_snd_voice_state::
	.ds SND_VOICE_COUNT * 8
; The absolute due times of the wait-listed voices (0 = not waited).
_snd_voice_due::
	.ds SND_VOICE_COUNT * 2
; The wait list (single-linked by voice index).
_snd_wait_head::
	.ds 1
_snd_wait_next::
	.ds SND_VOICE_COUNT
; The run list head.
_snd_voice_head::
	.ds 1
; The engine's working state.
_snd_elapsed::
	.ds 2
_snd_clock::
	.ds 2
_snd_min_due::
	.ds 2
_snd_remaining::
	.ds 2
_snd_cursor::
	.ds 2
_snd_wait::
	.ds 2
_snd_voice_index::
	.ds 1
_snd_next_voice::
	.ds 1
_snd_voice_tmp::
	.ds 1

	.area _CODE

; ---------------------------------------------------------------------------
; void snd_engine(uint16_t elapsed);  HL = ticks to advance
;   Advance the clock; rescan the wait list when the earliest due time
;   is reached; then walk the run list.
; ---------------------------------------------------------------------------
_snd_engine::
	ld	(_snd_elapsed), hl
	ld	de, (_snd_clock)
	add	hl, de
	ld	(_snd_clock), hl
	ld	de, (_snd_min_due)
	or	a
	sbc	hl, de			; HL = clock - min, C if clock < min
	jr	c, _walk
	call	_scan_wait_list
_walk:
	ld	a, (_snd_voice_head)
	cp	#SND_VOICE_NONE
	ret	z
_voice_loop:
	ld	(_snd_voice_index), a
	; HL = &_snd_voice_state[voice]
	ld	l, a
	ld	h, #0
	add	hl, hl
	add	hl, hl
	add	hl, hl
	ld	de, #_snd_voice_state
	add	hl, de
	ld	a, (hl)			; next (run list)
	ld	(_snd_next_voice), a
	inc	hl			; skip prev
	inc	hl
	ld	e, (hl)			; wlo
	inc	hl
	ld	d, (hl)			; whi
	inc	hl
	ld	c, (hl)			; clo
	inc	hl
	ld	b, (hl)			; chi
	inc	hl
	ld	a, (hl)			; flags
	ld	(_snd_wait), de
	ld	(_snd_cursor), bc
	and	#SND_F_PAUSED
	jr	z, _voice_ready
	; The pause flag resumes the voice in place: clear it and run this
	; tick's ops with the full elapsed remaining.
	ld	a, (hl)
	and	#(0xFF ^ SND_F_PAUSED)
	ld	(hl), a
	ld	hl, (_snd_elapsed)
	ld	(_snd_remaining), hl
	jp	_voice_ops
_voice_ready:
	ld	a, d
	or	e
	jr	z, _voice_full
	; A wait-listed voice resumed with a residual wait: consume it from
	; this tick's remaining (the residual never exceeds the elapsed).
	ld	hl, (_snd_elapsed)
	or	a
	sbc	hl, de			; HL = elapsed - wait
	ld	(_snd_remaining), hl
	jp	_voice_ops
_voice_full:
	ld	hl, (_snd_elapsed)
	ld	(_snd_remaining), hl
_voice_ops:
	ld	hl, (_snd_cursor)
_op_loop:
	ld	hl, (_snd_cursor)
	ld	a, (hl)
	cp	#SND_P_END
	jp	z, _op_end
	cp	#SND_P_PERIOD
	jp	z, _op_period
	cp	#SND_P_ATTEN
	jp	z, _op_atten
	cp	#SND_P_NOISE
	jp	z, _op_noise
	cp	#SND_P_WAIT8
	jp	z, _op_wait8
	; SND_P_WAIT16
	inc	hl
	ld	e, (hl)
	inc	hl
	ld	d, (hl)
	inc	hl
	ld	(_snd_cursor), hl
	jp	_op_wait_common
_op_wait8:
	inc	hl
	ld	e, (hl)
	ld	d, #0
	inc	hl
	ld	(_snd_cursor), hl
_op_wait_common:
	; DE = n.  n > remaining: park for the residual; else consume the
	; wait now and pause until the next tick (one step per tick).
	ld	hl, (_snd_remaining)
	or	a
	sbc	hl, de			; HL = remaining - n, C if remaining < n
	jp	c, _park_residual
	ld	(_snd_remaining), hl
	jp	_park_paused

; ---------------------------------------------------------------------------
; The park paths.
; ---------------------------------------------------------------------------
_park_paused:
	; set SND_F_PAUSED on the voice's slot and make it due next tick,
	; keeping the cursor past the WAIT so the resume stays in place
	call	_voice_slot_addr
	ld	de, #4
	add	hl, de
	ld	a, (_snd_cursor)
	ld	(hl), a
	inc	hl
	ld	a, (_snd_cursor + 1)
	ld	(hl), a
	inc	hl
	ld	a, (hl)
	or	#SND_F_PAUSED
	ld	(hl), a
	ld	hl, (_snd_clock)
	ld	(_snd_min_due), hl
	jp	_voice_next

_park_residual:
	; wait = n - remaining = -(remaining - n): negate HL
	ld	a, l
	cpl
	ld	l, a
	ld	a, h
	cpl
	ld	h, a
	inc	hl
	ld	(_snd_wait), hl
	; due = clock + wait
	ld	de, (_snd_clock)
	add	hl, de
	push	hl			; due
	; due[v] = due
	ld	a, (_snd_voice_index)
	add	a, a
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_voice_due
	add	hl, de
	pop	de			; DE = due
	ld	(hl), e
	inc	hl
	ld	(hl), d
	; wait[v] = wait (state slot)
	push	de			; due
	call	_voice_slot_addr
	inc	hl
	inc	hl
	ld	a, (_snd_wait)
	ld	(hl), a
	inc	hl
	ld	a, (_snd_wait + 1)
	ld	(hl), a
	inc	hl
	ld	a, (_snd_cursor)
	ld	(hl), a
	inc	hl
	ld	a, (_snd_cursor + 1)
	ld	(hl), a
	pop	de			; DE = due
	; unlink from the run list and join the wait list (both clobber
	; the registers, so the due time rides the stack)
	push	de			; due
	call	_voice_unlink
	call	_voice_wait_push
	pop	de			; DE = due
	; min_due = min(min_due, due)
	ld	hl, (_snd_min_due)
	ld	a, h
	and	l
	inc	a
	jr	z, _park_min_set
	or	a
	sbc	hl, de			; HL = min - due, C if min < due
	jr	c, _park_next
_park_min_set:
	ld	a, e
	ld	(_snd_min_due), a
	ld	a, d
	ld	(_snd_min_due + 1), a
_park_next:
	jp	_voice_next

_voice_next:
	ld	a, (_snd_next_voice)
	cp	#SND_VOICE_NONE
	ret	z
	jp	_voice_loop

; ---------------------------------------------------------------------------
; The wait-list scan: resume every due voice, track the next minimum.
; Runs only when clock >= min_due.  A = current, B = prev, DE = min.
; ---------------------------------------------------------------------------
_scan_wait_list:
	ld	de, #0xFFFF
	ld	b, #SND_VOICE_NONE
	ld	a, (_snd_wait_head)
_scan_loop:
	cp	#SND_VOICE_NONE
	jp	z, _scan_done
	push	bc			; prev
	push	de			; min
	push	af			; current voice
	; DE = due[v]
	add	a, a
	ld	l, a
	ld	h, #0
	ld	de, #_snd_voice_due
	add	hl, de
	ld	e, (hl)
	inc	hl
	ld	d, (hl)
	; due <= clock ?
	ld	hl, (_snd_clock)
	or	a
	sbc	hl, de			; HL = clock - due, C if clock < due
	jp	c, _scan_keep
	; ---- resume: w = elapsed - (clock - due) ----
	ex	de, hl			; DE = clock - due
	ld	hl, (_snd_elapsed)
	or	a
	sbc	hl, de			; HL = w
	pop	af			; A = voice
	ld	c, a			; C = voice
	push	hl			; w
	; wait[v] = w (state slot +2)
	ld	l, a
	ld	h, #0
	add	hl, hl
	add	hl, hl
	add	hl, hl
	ld	de, #_snd_voice_state + 2
	add	hl, de
	pop	de			; DE = w
	ld	(hl), e
	inc	hl
	ld	(hl), d
	; due[v] = 0
	ld	a, c
	add	a, a
	ld	l, a
	ld	h, #0
	ld	de, #_snd_voice_due
	add	hl, de
	xor	a
	ld	(hl), a
	inc	hl
	ld	(hl), a
	; link into the run list
	ld	a, c
	ld	(_snd_voice_index), a
	call	_voice_link
	; unlink from the wait list (prev in the saved BC)
	pop	de			; old min (dropped)
	pop	bc			; B = prev
	ld	a, c			; A = voice
	ld	l, a
	ld	h, #0
	ld	de, #_snd_wait_next
	add	hl, de
	ld	a, (hl)			; A = wait_next[v] (the new current)
	ld	d, a			; D = current
	ld	a, b			; A = prev
	cp	#SND_VOICE_NONE
	jr	z, _scan_resume_head
	ld	l, a
	ld	h, #0
	ld	de, #_snd_wait_next
	add	hl, de
	ld	(hl), d
	jr	_scan_resume_cont
_scan_resume_head:
	ld	a, d
	ld	(_snd_wait_head), a
_scan_resume_cont:
	; B already holds the old prev, which is the new current's prev
	ld	a, d
	jp	_scan_loop

_scan_keep:
	; not due: min = min(min, due)
	pop	af			; A = voice
	ld	c, a
	pop	hl			; HL = old min
	pop	bc			; B = prev
	ld	a, h
	and	l
	inc	a
	jr	z, _scan_keep_set	; old min == 0xFFFF
	push	hl
	or	a
	sbc	hl, de			; HL = min - due, C if min < due
	pop	hl
	jr	c, _scan_keep_done
_scan_keep_set:
	ex	de, hl			; HL = due
_scan_keep_done:
	ex	de, hl			; DE = min
	; the new current = wait_next[v]
	ld	e, c
	ld	d, #0
	ld	hl, #_snd_wait_next
	add	hl, de
	ld	a, (hl)
	jp	_scan_loop

_scan_done:
	ld	(_snd_min_due), de
	ret

; ---------------------------------------------------------------------------
; The run-list maintenance (links live in the state slots).
; ---------------------------------------------------------------------------
_voice_slot_addr:			; HL = &state[_snd_voice_index]
	ld	a, (_snd_voice_index)
	ld	l, a
	ld	h, #0
	add	hl, hl
	add	hl, hl
	add	hl, hl
	ld	de, #_snd_voice_state
	add	hl, de
	ret

_voice_link:
	call	_voice_slot_addr
	ld	a, (_snd_voice_head)
	ld	(hl), a			; next = head
	inc	hl
	ld	(hl), #SND_VOICE_NONE	; prev = NONE
	cp	#SND_VOICE_NONE
	jr	z, _link_head
	; state[old_head].prev = voice
	ld	l, a
	ld	h, #0
	add	hl, hl
	add	hl, hl
	add	hl, hl
	ld	de, #_snd_voice_state + 1
	add	hl, de
	ld	a, (_snd_voice_index)
	ld	(hl), a
_link_head:
	ld	a, (_snd_voice_index)
	ld	(_snd_voice_head), a
	ret

_voice_unlink:
	call	_voice_slot_addr
	ld	a, (hl)			; A = next
	inc	hl
	ld	b, (hl)			; B = prev
	dec	hl
	ld	c, a			; C = next
	ld	a, b
	cp	#SND_VOICE_NONE
	jr	nz, _unlink_mid
	ld	a, c
	ld	(_snd_voice_head), a
	jr	_unlink_prev
_unlink_mid:
	; state[prev].next = next
	ld	l, b
	ld	h, #0
	add	hl, hl
	add	hl, hl
	add	hl, hl
	ld	de, #_snd_voice_state
	add	hl, de
	ld	(hl), c
_unlink_prev:
	; state[next].prev = prev (when next exists)
	ld	a, c
	cp	#SND_VOICE_NONE
	ret	z
	ld	l, a
	ld	h, #0
	add	hl, hl
	add	hl, hl
	add	hl, hl
	ld	de, #_snd_voice_state + 1
	add	hl, de
	ld	(hl), b
	ret

_voice_wait_push:
	ld	a, (_snd_voice_index)
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_wait_next
	add	hl, de
	ld	a, (_snd_wait_head)
	ld	(hl), a
	ld	a, (_snd_voice_index)
	ld	(_snd_wait_head), a
	ret

_wait_list_remove:			; unlink _snd_voice_index from the wait list
	ld	b, #SND_VOICE_NONE	; prev
	ld	a, (_snd_wait_head)	; current
_wl_loop:
	cp	#SND_VOICE_NONE
	ret	z			; not found (cannot happen)
	ld	c, a			; C = current
	ld	a, (_snd_voice_index)
	cp	c
	ld	a, c
	jr	z, _wl_found
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_wait_next
	add	hl, de
	ld	b, c			; prev = current
	ld	a, (hl)
	jr	_wl_loop
_wl_found:
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_wait_next
	add	hl, de
	ld	a, (hl)			; A = wait_next[v]
	ld	d, a			; D = next
	ld	a, b
	cp	#SND_VOICE_NONE
	jr	z, _wl_head
	ld	l, a
	ld	h, #0
	ld	de, #_snd_wait_next
	add	hl, de
	ld	(hl), d
	ret
_wl_head:
	ld	a, d
	ld	(_snd_wait_head), a
	ret

; ---------------------------------------------------------------------------
; The opcode handlers; the cursor is in _snd_cursor, HL = its address.
; ---------------------------------------------------------------------------
_op_end:
	call	_voice_unlink
	ld	a, (_snd_voice_index)
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_voice_active
	add	hl, de
	ld	(hl), #0
	; clear the pause flag (a running voice's slot)
	call	_voice_slot_addr
	ld	de, #6
	add	hl, de
	ld	a, (hl)
	and	#(0xFF ^ SND_F_PAUSED)
	ld	(hl), a
	jp	_voice_next

_op_period:
	ld	hl, (_snd_cursor)
	inc	hl
	ld	e, (hl)
	inc	hl
	ld	d, (hl)			; DE = period
	inc	hl
	ld	(_snd_cursor), hl
	push	de
	call	_voice_period_addr
	pop	de
	ld	(hl), e
	inc	hl
	ld	(hl), d
	call	_queue_tone
	ld	a, #1
	ld	(_snd_dirty), a
	jp	_op_loop

_op_atten:
	ld	hl, (_snd_cursor)
	inc	hl
	ld	c, (hl)			; C = the linear level
	inc	hl
	ld	(_snd_cursor), hl
	ld	a, (_snd_voice_index)
	ld	e, a
	ld	d, #0			; DE = the voice
	ld	hl, #_snd_voice_level
	add	hl, de
	ld	(hl), c			; level[v] = level
	ld	hl, #_snd_desired_atten
	add	hl, de
	push	hl
	ld	b, d			; BC = the level
	ld	hl, #_snd_level_table
	add	hl, bc
	ld	a, (hl)			; A = the attenuation
	pop	hl
	ld	(hl), a			; desired_atten[v]
	; queue the attenuation slot: chip = v >> 2, slot = 4 + (v & 3)
	ld	a, e
	and	#0x03
	add	a, #4
	ld	l, a
	ld	a, e
	rrca
	rrca
	and	#0x3F
	call	_snd_queue_push
	ld	a, #1
	ld	(_snd_dirty), a
	jp	_op_loop

_op_noise:
	ld	hl, (_snd_cursor)
	inc	hl
	ld	a, (hl)
	inc	hl
	ld	(_snd_cursor), hl
	or	#0xE0
	ld	c, a			; C = the desired byte
	ld	a, (_snd_voice_index)
	srl	a
	srl	a			; chip
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_desired_noise
	add	hl, de
	ld	(hl), c
	ld	hl, #_snd_noise_force
	add	hl, de
	ld	(hl), #1
	ld	l, #3			; the noise slot
	ld	a, e			; chip
	call	_snd_queue_push
	ld	a, #1
	ld	(_snd_dirty), a
	jp	_op_loop

; The changed-slot queue entries (slot 0-7 within the chip's queue).
_queue_tone:
	ld	a, (_snd_voice_index)
	ld	e, a
	and	#0x03			; the tone sub-voice
	ld	l, a
	ld	a, e
	srl	a
	srl	a			; chip
	jp	_snd_queue_push
_queue_atten:
	ld	a, (_snd_voice_index)
	ld	e, a
	and	#0x03
	add	a, #4			; the attenuation sub-voice
	ld	l, a
	ld	a, e
	srl	a
	srl	a			; chip
	jp	_snd_queue_push

; ---------------------------------------------------------------------------
; void snd_voice_install(uint8_t voice);  A = voice
; ---------------------------------------------------------------------------
_snd_voice_install::
	di
	ld	(_snd_voice_index), a
	; zero the slot's wait, cursor and flags; next/prev set by the link
	call	_voice_slot_addr
	inc	hl
	inc	hl
	xor	a
	ld	(hl), a
	inc	hl
	ld	(hl), a
	inc	hl
	ld	(hl), a
	inc	hl
	ld	(hl), a
	inc	hl
	ld	(hl), a
	; cursor = &_snd_programs[voice] = base + voice * 86
	ld	a, (_snd_voice_index)
	ld	e, a
	ld	d, #0
	sla	e
	rl	d			; *2
	push	de
	sla	e
	rl	d			; *4
	push	de
	sla	e
	rl	d
	sla	e
	rl	d			; *16
	push	de
	sla	e
	rl	d
	sla	e
	rl	d			; *64
	ex	de, hl
	pop	de
	add	hl, de			; *80
	pop	de
	add	hl, de			; *84
	pop	de
	add	hl, de			; *86
	ld	de, #_snd_programs
	add	hl, de
	; store the cursor into the slot (+4)
	push	hl
	call	_voice_slot_addr
	pop	de
	inc	hl
	inc	hl
	inc	hl
	inc	hl
	ld	(hl), e
	inc	hl
	ld	(hl), d
	; due = 0
	ld	a, (_snd_voice_index)
	add	a, a
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_voice_due
	add	hl, de
	xor	a
	ld	(hl), a
	inc	hl
	ld	(hl), a
	; active = 1, level = 0
	ld	a, (_snd_voice_index)
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_voice_active
	add	hl, de
	ld	(hl), #1
	ld	hl, #_snd_voice_level
	add	hl, de
	ld	(hl), #0
	call	_voice_link
	ei
	ret

; ---------------------------------------------------------------------------
; void snd_voice_clear(uint8_t voice);  A = voice
; ---------------------------------------------------------------------------
_snd_voice_clear::
	di
	ld	(_snd_voice_index), a
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_voice_active
	add	hl, de
	ld	a, (hl)
	or	a
	jr	z, _clear_idle
	; installed: a waited voice (due != 0) leaves the wait list, a
	; running or paused voice leaves the run list
	ld	a, (_snd_voice_index)
	add	a, a
	ld	l, a
	ld	h, #0
	ld	de, #_snd_voice_due
	add	hl, de
	ld	a, (hl)
	inc	hl
	or	(hl)
	jr	nz, _clear_waited
	call	_voice_unlink
	jr	_clear_idle
_clear_waited:
	call	_wait_list_remove
	ld	a, (_snd_voice_index)
	add	a, a
	ld	l, a
	ld	h, #0
	ld	de, #_snd_voice_due
	add	hl, de
	xor	a
	ld	(hl), a
	inc	hl
	ld	(hl), a
_clear_idle:
	; slot: wait = 0, flags = 0 (the cursor is left stale)
	call	_voice_slot_addr
	inc	hl
	inc	hl
	xor	a
	ld	(hl), a
	inc	hl
	ld	(hl), a
	inc	hl
	inc	hl
	ld	(hl), a
	; active = 0, level = 0
	ld	a, (_snd_voice_index)
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_voice_active
	add	hl, de
	ld	(hl), #0
	ld	hl, #_snd_voice_level
	add	hl, de
	ld	(hl), #0
	; mute the desired attenuation and queue the slot
	call	_voice_atten_addr
	ld	(hl), #0x0F
	call	_queue_atten
	ld	a, #1
	ld	(_snd_dirty), a
	ei
	ret

; The desired-state addresses for the current voice.
_voice_period_addr:
	ld	a, (_snd_voice_index)
	ld	b, a
	and	#0xFC			; chip * 4
	ld	c, a
	srl	c			; chip * 2
	add	a, c			; chip * 6
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_desired_period
	add	hl, de
	ld	a, b
	and	#0x03
	add	a, a
	ld	e, a
	ld	d, #0
	add	hl, de
	ret

_voice_atten_addr:
	ld	a, (_snd_voice_index)
	ld	l, a
	ld	h, #0
	ld	de, #_snd_desired_atten
	add	hl, de
	ret
