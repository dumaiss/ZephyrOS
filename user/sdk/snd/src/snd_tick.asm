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
	.globl _snd_output_flush
	.globl _snd_overruns

SND_TICK_CATCHUP_MAX = 16

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
	pop	bc			; C = the bounded count (1..16)
	; the ZVG advances by the same count: one decoder tick each
	ld	a, c
	or	a
	jr	z, _tick_flush
_tick_zvg:
	push	bc
	call	_snd_zvg_tick
	pop	bc
	dec	c
	jr	nz, _tick_zvg
_tick_flush:
	call	_snd_output_flush
	ld	de, #0
	ret
