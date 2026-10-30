; -----------------------------------------------------------------------------
; libsnd: the ZTR-only tick (user/sdk/snd/src).
;
;   int snd_tick_ztr(uint16_t elapsed_ticks);   HL = ticks, DE = 0
;
; snd_tick for a program that plays ZTR songs and nothing else: the same
; catch-up bound and overrun count, the ZTR engine's ticks and one shadow
; flush, without the manual voice engine or the ZVG decoder.  Its own
; member, so such a program links neither (and a placed program keeps
; neither in its handler's resident closure, architecture 7.2).
; -----------------------------------------------------------------------------

	.module snd_tick_ztr

	.globl _snd_tick_ztr
	.globl _snd_ztr_tick
	.globl _snd_ztr_state
	.globl _snd_output_flush
	.globl _snd_overruns

SND_TICK_CATCHUP_MAX = 16
SND_ZTR_PLAYING     = 3
SND_ZTR_UNDERRUN    = 6

	.area _CODE

_snd_tick_ztr::
	; HL = elapsed
	ld	de, #SND_TICK_CATCHUP_MAX
	push	hl
	or	a
	sbc	hl, de			; elapsed - max, C if elapsed < max
	pop	hl
	jr	c, 1$
	jr	z, 1$
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
1$:
	ld	c, l
	ld	a, c
	or	a
	jr	z, 3$
	ld	a, (_snd_ztr_state)
	cp	#SND_ZTR_PLAYING
	jr	z, 2$
	cp	#SND_ZTR_UNDERRUN
	jr	nz, 3$
2$:
	push	bc
	call	_snd_ztr_tick
	pop	bc
	dec	c
	jr	nz, 2$
3$:
	call	_snd_output_flush
	ld	de, #0
	ret
