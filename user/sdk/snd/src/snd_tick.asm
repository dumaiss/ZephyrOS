; -----------------------------------------------------------------------------
; libsnd tick dispatch (user/sdk/snd/src): snd_tick's catch-up bound, the
; engine call and the flush.
;
;   int snd_tick(uint16_t elapsed_ticks);   HL = ticks, DE = 0
;
; The tick advances every active voice program, and an active ZVG, by
; at most SND_TICK_CATCHUP_MAX ticks; the excess counts as an overrun
; (snd_overruns) and is never replayed.  Then one shadow flush.
; -----------------------------------------------------------------------------

	.module snd_tick

	.globl _snd_tick
	.globl _snd_engine
	.globl _snd_zvg_tick
	.globl _snd_zvg_state
	.globl _snd_ztr_tick
	.globl _snd_ztr_state
	.globl _snd_output_flush
	.globl _snd_overruns

SND_TICK_CATCHUP_MAX = 16
SND_ZVG_PLAYING     = 2
SND_ZVG_UNDERRUN    = 3
SND_ZTR_PLAYING     = 3
SND_ZTR_UNDERRUN    = 6

	.area _CODE

_snd_tick::
	; HL = elapsed
	ld	de, #SND_TICK_CATCHUP_MAX
	push	hl
	or	a
	sbc	hl, de			; elapsed - max, C if elapsed < max
	pop	hl
	jr	c, _tick_bounded
	jr	z, _tick_bounded
	; the excess is never replayed
	push	hl
	ld	de, #SND_TICK_CATCHUP_MAX
	or	a
	sbc	hl, de
	ex	de, hl
	ld	hl, (_snd_overruns)
	add	hl, de
	ld	(_snd_overruns), hl
	pop	hl
	ld	hl, #SND_TICK_CATCHUP_MAX
_tick_bounded:
	push	hl
	call	_snd_engine
	pop	hl			; HL = the bounded count (1..16)
	; an active ZVG advances by the same count: one decoder tick each
	ld	c, l
	ld	a, c
	or	a
	jr	z, _tick_flush
	ld	a, (_snd_zvg_state)
	cp	#SND_ZVG_PLAYING
	jr	z, _tick_zvg
	cp	#SND_ZVG_UNDERRUN
	jr	nz, _tick_ztr_gate
_tick_zvg:
	push	bc
	call	_snd_zvg_tick
	pop	bc
	dec	c
	jr	nz, _tick_zvg
_tick_ztr_gate:
	; an active ZTR advances by the same count: one engine tick each
	ld	c, l
	ld	a, c
	or	a
	jr	z, _tick_flush
	ld	a, (_snd_ztr_state)
	cp	#SND_ZTR_PLAYING
	jr	z, _tick_ztr
	cp	#SND_ZTR_UNDERRUN
	jr	nz, _tick_flush
_tick_ztr:
	push	bc
	call	_snd_ztr_tick
	pop	bc
	dec	c
	jr	nz, _tick_ztr
_tick_flush:
	call	_snd_output_flush
	ld	de, #0
	ret
