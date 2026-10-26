; -----------------------------------------------------------------------------
; libsnd ZVG decoder (user/sdk/snd/src): the assembly half of the ZVG tick.
;
;   void snd_zvg_tick(void);
;
; One playback tick: the wait counter, then complete commands from the
; ring up to the next wait or the end.  WRITE and WRITE RUN bytes go to
; the chip - DIRECT straight to its port, BATCH appended to the tick's
; batch - so the chip bypasses the shadow (the stop path invalidates
; it).  An incomplete command at the head is an underrun: mute, count,
; resume when snd_service refills; never a partial command.  A
; malformed opcode (or a stream that ends without its END) is an error
; state, muted, with the offset kept.  END finishes playback: DONE,
; with no writes (the reference's last tick is silent).
;
; The ring indices and the publication contract are bytes, so the
; producer/consumer pair needs no masking, and the ring is 256-byte
; aligned: a command's bytes are page:index, stepped with `inc l`, so a
; command that straddles the ring's end wraps to its start.
; -----------------------------------------------------------------------------

	.module snd_zvg_dec

	.globl _snd_zvg_tick
	.globl _snd_zvg_ring_page
	.globl _snd_zvg_head
	.globl _snd_zvg_tail
	.globl _snd_zvg_chip
	.globl _snd_zvg_state
	.globl _snd_zvg_wait
	.globl _snd_zvg_consumed
	.globl _snd_zvg_ticks
	.globl _snd_zvg_writes
	.globl _snd_zvg_underruns
	.globl _snd_zvg_error_offset
	.globl _snd_zvg_error_op
	.globl _snd_zvg_eof
	.globl _snd_output_mode
	.globl _snd_ports
	.globl _snd_batch
	.globl _snd_batch_count
	.globl snd_batch_send

SND_ZVG_RING      = 256
SND_BATCH_MAX     = 60
SND_ZVG_HEADER    = 16
SND_ZVG_PLAYING   = 2
SND_ZVG_UNDERRUN  = 3
SND_ZVG_ERROR     = 4
SND_ZVG_DONE      = 5

SND_P_END         = 0x00
SND_P_WRITE       = 0x01
SND_P_WAIT16      = 0x02
SND_WAIT_SHORT_LO = 0x40
SND_WAIT_SHORT_HI = 0x80
SND_RUN_LO        = 0x80
SND_RUN_HI        = 0x90

	.area _DATA

; The shared ZVG state lives in snd_zvg.c (the .globl block above makes
; this module's references resolve there); only the decoder's per-tick
; working bytes are module-local.
_zvg_opcode::
	.ds 1
_zvg_writes_left::
	.ds 1

	.area _CODE

; ---------------------------------------------------------------------------
; One byte to the ZVG chip: A = value.  DIRECT: OUT (C),A with the
; chip's port; BATCH: append {chip, value} to the tick's batch.
; ---------------------------------------------------------------------------
_zvg_write:
	ld	b, a			; the value
	ld	a, (_snd_output_mode)
	or	a
	jr	nz, _zvg_write_batch
	ld	a, (_snd_zvg_chip)
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_ports
	add	hl, de
	ld	c, (hl)
	ld	a, b
	out	(c), a
	ret
_zvg_write_batch:
	; a full batch goes out first (catch-up can run 16 ticks at once)
	ld	a, (_snd_batch_count)
	cp	#SND_BATCH_MAX
	jr	c, _zvg_batch_room
	push	bc
	push	hl
	call	snd_batch_send
	pop	hl
	pop	bc
_zvg_batch_room:
	ld	a, (_snd_batch_count)
	add	a, a
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_batch
	add	hl, de
	ld	a, (_snd_zvg_chip)
	ld	(hl), a
	inc	hl
	ld	a, b
	ld	(hl), a
	ld	a, (_snd_batch_count)
	inc	a
	ld	(_snd_batch_count), a
	ret

; The chip's mute: the three tone attenuators.  (The noise channel
; follows channel 3's attenuator, so 9F/BF/DF silences all four.)
_zvg_mute:
	ld	a, #0x9F
	call	_zvg_write
	ld	a, #0xBF
	call	_zvg_write
	ld	a, #0xDF
	jp	_zvg_write

; ---------------------------------------------------------------------------
; void snd_zvg_tick(void);
; ---------------------------------------------------------------------------
_snd_zvg_tick::
	ld	a, (_snd_zvg_state)
	cp	#SND_ZVG_PLAYING
	jr	z, _zvg_run
	cp	#SND_ZVG_UNDERRUN
	ret	nz
_zvg_run:
	ld	hl, (_snd_zvg_ticks)
	inc	hl
	ld	(_snd_zvg_ticks), hl
	ld	hl, (_snd_zvg_wait)
	ld	a, h
	or	l
	jr	z, _zvg_cmd
	dec	hl
	ld	(_snd_zvg_wait), hl
	ld	a, h
	or	l
	ret	nz
	jp	_zvg_cmd		; the wait elapsed: continue this tick

; The command loop: B = head, C = available, HL = &ring[head].
_zvg_cmd:
	ld	a, (_snd_zvg_head)
	ld	b, a
	ld	a, (_snd_zvg_tail)
	sub	b			; available, mod 256
	jr	nz, _zvg_decode
	; the ring is empty: a truncated stream if the reader is done,
	; otherwise an underrun the next refill repairs
	ld	a, (_snd_zvg_eof)
	or	a
	jr	nz, _zvg_truncated
	jp	_zvg_underrun
_zvg_decode:
	ld	c, a			; C = available
	ld	a, (_snd_zvg_ring_page)
	ld	h, a
	ld	l, b			; HL = &ring[head]
	ld	a, (hl)
	ld	(_zvg_opcode), a
	or	a
	jp	z, _zvg_end_op
	cp	#SND_P_WRITE
	jp	z, _zvg_write_op
	cp	#SND_P_WAIT16
	jp	z, _zvg_wait16_op
	cp	#SND_WAIT_SHORT_LO
	jp	c, _zvg_bad
	cp	#SND_WAIT_SHORT_HI
	jp	c, _zvg_wait_short
	cp	#SND_RUN_HI
	jp	c, _zvg_run_op
	jp	_zvg_bad

; --- the incomplete-command paths ----------------------------------------
_zvg_incomplete:
	ld	a, (_snd_zvg_eof)
	or	a
	jr	nz, _zvg_truncated
	call	_zvg_underrun
	ret

_zvg_truncated:
	; the declared stream ended without completing the command at the
	; head: report the head opcode, or FFh when the ring is empty
	ld	hl, (_snd_zvg_consumed)
	ld	de, #SND_ZVG_HEADER
	add	hl, de
	ld	(_snd_zvg_error_offset), hl
	ld	a, #0xFF
	ld	(_snd_zvg_error_op), a
	ld	a, (_snd_zvg_head)
	ld	l, a
	ld	a, (_snd_zvg_tail)
	sub	l
	jr	z, _zvg_truncated_mute
	ld	a, (_snd_zvg_ring_page)
	ld	h, a			; HL = &ring[head] (L = head)
	ld	a, (hl)
	ld	(_snd_zvg_error_op), a
_zvg_truncated_mute:
	ld	a, #SND_ZVG_ERROR
	ld	(_snd_zvg_state), a
	jp	_zvg_mute

_zvg_underrun:
	ld	hl, (_snd_zvg_underruns)
	inc	hl
	ld	(_snd_zvg_underruns), hl
	ld	a, #SND_ZVG_UNDERRUN
	ld	(_snd_zvg_state), a
	call	_zvg_mute
	ret

_zvg_bad:
	ld	hl, (_snd_zvg_consumed)
	ld	de, #SND_ZVG_HEADER
	add	hl, de
	ld	(_snd_zvg_error_offset), hl
	ld	a, (_zvg_opcode)
	ld	(_snd_zvg_error_op), a
	ld	a, #SND_ZVG_ERROR
	ld	(_snd_zvg_state), a
	call	_zvg_mute
	ret

; --- END: done, like the reference (no writes on the END tick) ------------
_zvg_end_op:
	ld	a, b
	inc	a
	ld	(_snd_zvg_head), a
	ld	hl, (_snd_zvg_consumed)
	inc	hl
	ld	(_snd_zvg_consumed), hl
	ld	a, #SND_ZVG_DONE
	ld	(_snd_zvg_state), a
	ret

; --- WRITE b --------------------------------------------------------------
_zvg_write_op:
	ld	a, c
	cp	#2
	jr	c, _zvg_incomplete
	inc	l
	ld	a, (hl)		; the byte
	push	bc
	call	_zvg_write
	pop	bc
	ld	hl, (_snd_zvg_writes)
	inc	hl
	ld	(_snd_zvg_writes), hl
	ld	a, b
	add	a, #2
	ld	(_snd_zvg_head), a
	ld	hl, (_snd_zvg_consumed)
	ld	de, #2
	add	hl, de
	ld	(_snd_zvg_consumed), hl
	jp	_zvg_cmd

; --- WAIT16 lo hi ---------------------------------------------------------
_zvg_wait16_op:
	ld	a, c
	cp	#3
	jp	c, _zvg_incomplete
	inc	l
	ld	e, (hl)
	inc	l
	ld	d, (hl)
	ld	a, d
	or	e
	jr	z, _zvg_bad		; a zero wait is malformed
	ld	(_snd_zvg_wait), de
	ld	a, b
	add	a, #3
	ld	(_snd_zvg_head), a
	ld	hl, (_snd_zvg_consumed)
	ld	de, #3
	add	hl, de
	ld	(_snd_zvg_consumed), hl
	ret

; --- WAIT n (40h-7Fh) -----------------------------------------------------
_zvg_wait_short:
	ld	a, (_zvg_opcode)
	and	#0x3F
	inc	a
	ld	e, a
	ld	d, #0
	ld	(_snd_zvg_wait), de
	ld	a, b
	inc	a
	ld	(_snd_zvg_head), a
	ld	hl, (_snd_zvg_consumed)
	inc	hl
	ld	(_snd_zvg_consumed), hl
	ret

; --- WRITE RUN (80h-8Fh) --------------------------------------------------
_zvg_run_op:
	ld	a, (_zvg_opcode)
	and	#0x0F
	inc	a			; the write count
	ld	(_zvg_writes_left), a
	inc	a			; needed = writes + 1
	ld	d, a			; D = needed
	ld	a, c
	cp	d
	jp	c, _zvg_incomplete
	inc	l			; the first byte
_zvg_run_loop:
	ld	a, (hl)
	inc	l
	push	bc
	push	de
	push	hl
	call	_zvg_write
	pop	hl
	pop	de
	pop	bc
	ld	a, (_zvg_writes_left)
	dec	a
	ld	(_zvg_writes_left), a
	jr	nz, _zvg_run_loop
	ld	hl, (_snd_zvg_writes)
	ld	a, (_zvg_opcode)
	and	#0x0F
	inc	a
	ld	e, a
	ld	d, #0
	add	hl, de
	ld	(_snd_zvg_writes), hl
	; consume the opcode plus its bytes
	ld	a, (_zvg_opcode)
	and	#0x0F
	add	a, #2
	ld	e, a
	ld	d, #0
	ld	a, b
	add	a, e
	ld	(_snd_zvg_head), a
	ld	hl, (_snd_zvg_consumed)
	add	hl, de
	ld	(_snd_zvg_consumed), hl
	jp	_zvg_cmd
