; -----------------------------------------------------------------------------
; libsnd: the PCM real-time handler (docs/snd-sdk.md section 12).
;
; The IO controller's RT clock runs this handler through the Z80's NMI at
; the stream's sample rate (kos/rt.h).  An NMI ignores DI, so no masked
; section of the system can hold a sample back; the handler can arrive in
; any mapping, so it and everything it touches live in the RT region of
; common memory, which the nucleus lends to the interrupt's owner:
;
;   F000h-FBFFh   the ring: 12 pages of 256 samples
;   FC00h-FC0Fh   counters (below)
;   FC10h-        the handler, copied from snd_pcm_rt_image by snd_pcm_play
;
; The handler's time is the board's lane gap (rt_gap): the IO controller
; holds its lanes around every tick, so it is kept short.  Its own code
; holds the ring state - it must run at SND_PCM_RT_AT:
;   rd     the operand of its LD A,(nn): the next sample's address.  Only
;          the handler writes it; the foreground reads rd's high byte (one
;          byte: atomic).
;   wr     the operand of its CP n: the first page the foreground has not
;          filled.  Only the foreground writes it, after the page's bytes
;          are in place.
; It checks the ring only when a page ends: it moves to the next page
; unless that page is wr, and otherwise plays the page it finished again
; (an underrun, or after the stream's closing silence page, the drain),
; counting those pages.  The foreground fills at most 11 of the 12 pages
; and never writes the page rd names.
;
; Cost (T-states, the NMI acknowledge's 11 included): a sample 107, a
; sample that ends a page 175 (more on an underrun); the lane gap must
; cover the page's end (SND_PCM_GAP_US).  It pushes only AF
; (within KOS_RT_STACK_MAX) and touches no other register.
; -----------------------------------------------------------------------------

	.module snd_pcm_rt

	.globl _snd_pcm_rt_image, _snd_pcm_rt_image_end
	.globl _snd_pcm_rt_rd, _snd_pcm_rt_out, _snd_pcm_rt_wr

; Keep in step with snd_pcm.c.
SND_PCM_RT_AT   = 0xFC10        ; where the image runs
PCM_RD_LO       = SND_PCM_RT_AT + 2     ; rd: the LD A,(nn) operand
PCM_RD_HI       = SND_PCM_RT_AT + 3
PCM_EOF         = 0xFC03        ; byte: non-zero once the stream is in
PCM_UNDER       = 0xFC04        ; word: pages replayed before EOF
PCM_DRAIN       = 0xFC06        ; word: pages replayed after EOF
PCM_RING_FIRST  = 0xF0          ; first ring page
PCM_RING_END    = 0xFC          ; exclusive: the state page

	.area _CODE

_snd_pcm_rt_image::
	push	af				; 11
_snd_pcm_rt_rd::
	ld	a,(0xF000)			; 13  rd: patched as it plays
_snd_pcm_rt_out::
	out	(0xE4),a			; 11  the DAC port, patched at play
	ld	a,(PCM_RD_LO)			; 13
	inc	a				; 4
	ld	(PCM_RD_LO),a			; 13
	jr	z,rt_page			; 7
	pop	af				; 10
	retn					; 14
rt_page:
	ld	a,(PCM_RD_HI)			; 13
	inc	a				; 4
	cp	#PCM_RING_END			; 7
	jr	nz,rt_wr			; 12
	ld	a,#PCM_RING_FIRST
_snd_pcm_rt_wr::
rt_wr:
	cp	#0				; 7  wr: the foreground's
	jr	z,rt_starved			; 7
	ld	(PCM_RD_HI),a			; 13
	pop	af				; 10
	retn					; 14
rt_starved:
	ld	a,(PCM_EOF)
	or	a
	jr	nz,rt_drain
	ld	a,(PCM_UNDER)
	inc	a
	ld	(PCM_UNDER),a
	jr	nz,rt_done
	ld	a,(PCM_UNDER+1)
	inc	a
	ld	(PCM_UNDER+1),a
rt_done:
	pop	af
	retn
rt_drain:
	ld	a,(PCM_DRAIN)
	inc	a
	ld	(PCM_DRAIN),a
	jr	nz,rt_done
	ld	a,(PCM_DRAIN+1)
	inc	a
	ld	(PCM_DRAIN+1),a
	jr	rt_done
_snd_pcm_rt_image_end::
