; -----------------------------------------------------------------------------
; libsnd ZTR engine (user/sdk/snd/src): the assembly half of the ZTR tick.
;
;   void snd_ztr_tick(void);          one playback tick
;   void snd_ztr_mute(void);          the mask's voices to attenuation 15
;   void snd_ztr_restore(void);       re-render from the held state
;   void snd_ztr_reset_channels(void); clear the engine's channel storage
;
; The C half (snd_ztr.c) owns open/validate, the resident tables and the
; foreground producer of the prepared-row queue.  One playback tick is a
; port of the preserved reference player (tests/snd/oracle):
; at the row's first tick, consume the due head record (order, row) and
; apply its already-decoded per-channel events (the reference player's
; begin_row and decode_row); otherwise a due record that is not resident
; is an underrun - mute the ZTR's voices, count, and resume at the row
; once snd_service has published it.  Effects/notation, active macros
; and rendering are fused into one channel-order walk; work flags skip
; idle channels without changing the reference ordering.  DIRECT with
; a valid shadow compares and writes period/noise/attenuation inline;
; BATCH and invalid-shadow paths use the snd_put_* primitives.  The
; stop-song effect ends the song in DONE deterministically, with no
; writes on that tick.
;
; Channel state lives in assembly-owned storage: one core page and a
; second page for the effect/notation half (channel c's bytes are
; page:c<<4, reached with L arithmetic and the companion-page `inc h`),
; and one page per macro slot.  macro_position remains an item index;
; each bound slot caches its immutable loop, release, count and initial
; delay while retaining the reference's bounding semantics.  The divisor table splits into a lo page and the
; following hi page; the loudness page maps base<<4|macro to
; base*macro/15, removing the oracle's multiply and divide.  Only
; fine_pitch multiplies, by a bounded 6-step shift-and-add.
; -----------------------------------------------------------------------------

	.module snd_ztr_dec

	.globl _snd_ztr_tick
	.globl _snd_ztr_mute
	.globl _snd_ztr_restore
	.globl _snd_ztr_reset_channels
	.globl _snd_ztr_state
	.globl _snd_ztr_speed
	.globl _snd_ztr_pattern_length
	.globl _snd_ztr_order_count
	.globl _snd_ztr_head
	.globl _snd_ztr_tail
	.globl _snd_ztr_queue_addrs
	.globl _snd_ztr_order
	.globl _snd_ztr_row
	.globl _snd_ztr_tick_in_row
	.globl _snd_ztr_mask
	.globl _snd_ztr_underruns
	.globl _snd_ztr_core_page
	.globl _snd_ztr_core2_page
	.globl _snd_ztr_macro_page
	.globl _snd_ztr_div_lo_page
	.globl _snd_ztr_loud_page
	.globl _snd_ztr_instr_page
	.globl _snd_ztr_macro_base
	.globl _snd_desc_page
	.globl _snd_output_mode
	.globl _snd_all_valid
	.globl _snd_fast
	.globl snd_put_period
	.globl snd_put_atten
	.globl snd_put_noise

SND_ZTR_QUEUE_ROWS = 3
SND_ZTR_SLOT_BYTES = 10

SND_ZTR_EMPTY     = 0
SND_ZTR_PREPARING = 1
SND_ZTR_READY     = 2
SND_ZTR_PLAYING   = 3
SND_ZTR_PAUSED    = 4
SND_ZTR_DONE      = 5
SND_ZTR_UNDERRUN  = 6
SND_ZTR_ERROR     = 7

; The event bits, the modifiers and the native effects (snd_private.h).
EV_NOTE      = 0x80
EV_INSTRUMENT = 0x40
EV_VOLUME    = 0x20
EV_EFFECT    = 0x10
EV_OFF       = 0x08
EV_RELEASE   = 0x04
EV_LEGATO    = 0x02

MOD_RESET    = 0x80
MOD_ARP_SET  = 0x40
MOD_ARP_CLEAR = 0x20
MOD_HAIRPIN_SET = 0x10
MOD_HAIRPIN_CLEAR = 0x08

FX_ARPEGGIO  = 1
FX_VOL_SLIDE = 2
FX_LEGATO    = 3
FX_VOL_DEC   = 4
FX_FAST_VOL  = 5
FX_STOP      = 6

; The core page: channel c at core_page:c<<4.
ZC_WORK        = 0	; bit0 effect, bit1 notation, bits2-6 macro slots,
			; bit7 dirty-any
ZC_DIRTY       = 1	; bit0 pitch, bit1 volume, bit2 noise, bit3 phase
ZC_FLAGS       = 2	; bit0 active, bit1 released, bit2 legato,
			; bit3 notation arpeggio
ZC_NOTE        = 3
ZC_INSTRUMENT  = 4
ZC_BASE_VOL    = 5
ZC_MACRO_VOL   = 6
ZC_EFFECT      = 7
ZC_EFFECT_PARAM = 8
ZC_ARP_LO      = 9
ZC_ARP_FLAGS   = 10	; bit0 fixed note
ZC_PITCH_LO    = 11
ZC_PITCH_HI    = 12
ZC_DUTY        = 13
ZC_PHASE       = 14
ZC_ARP_HI      = 15

; The effect/notation half (core2 page).
ZC2_EFFECT_PHASE = 0
ZC2_NOT_ARP    = 1
ZC2_NOT_PHASE  = 2
ZC2_HAIR_RATE  = 3
ZC2_HAIR_OFF   = 4
ZC2_HAIR_CEIL  = 5
; The oracle's register cache, kept as channel fields (psg.c).
ZC2_LAST_PER_LO = 6
ZC2_LAST_PER_HI = 7
ZC2_LAST_ATT   = 8
ZC2_LAST_NOISE = 9

; One macro slot's record (the macro page): channel c at page:c<<4.
ZM_POS_LO      = 0	; macro_position, an item index into the macro
ZM_POS_HI      = 1
ZM_TIMER       = 2
ZM_DELAY       = 3
ZM_MACRO_LO    = 4	; the macro's address in the resident area
ZM_MACRO_HI    = 5
ZM_LOOP_LO     = 6	; cached literal header bounds (Phase 2)
ZM_LOOP_HI     = 7
ZM_RELEASE_LO  = 8
ZM_RELEASE_HI  = 9
ZM_COUNT_LO    = 10
ZM_COUNT_HI    = 11
ZM_INIT_DELAY  = 12	; cached header byte +4 for note_on reset

CF_ACTIVE      = 0x01
CF_RELEASED    = 0x02
CF_LEGATO      = 0x04
CF_NOT_ARP     = 0x08

DY_PITCH       = 0x01
DY_VOLUME      = 0x02
DY_NOISE       = 0x04
DY_PHASE       = 0x08

; Descriptor fields (snd_out.asm).
VD_ATT_DESIRED = 6
VD_ATT_SHADOW  = 7
VD_ATT_LATCH   = 8
VD_PORT        = 9
VD_REG_LATCH   = 10
VD_DESIRED_REG = 12
VD_SHADOW_REG  = 14

	.area _DATA

_zt_sign::
	.ds 1		; fine_pitch's sign (0 down, 1 up)
_zt_slot::
	.ds 1		; the macro slot under work
_zt_chl::
	.ds 1		; the channel's L (c << 4)
_zt_stepped::
	.ds 1		; a macro item ran
_zt_value::
	.ds 2		; the macro item's signed value
_zt_vflags::
	.ds 1		; the macro item's value flags
_zt_pos::
	.ds 2		; bound_macro_position's position
_zt_loop::
	.ds 2		; the macro header's loop index
_zt_release::
	.ds 2		; the macro header's release index
_zt_count::
	.ds 2		; the macro header's item count
_zt_macro_ptr::
	.ds 2		; the macro's address in the resident area
_zt_sloth::
	.ds 1		; the slot page of the macro under work
_zt_delay::
	.ds 1		; a macro header byte carried across the page switch
_zt_instr_num::
	.ds 1
_zt_entry_l::
	.ds 1
_zt_head::
	.ds 2
_zt_event_mask::
	.ds 1
_zt_event_note::
	.ds 1
_zt_event_instr::
	.ds 1
_zt_event_vol::
	.ds 1
_zt_event_effect::
	.ds 1
_zt_event_param::
	.ds 1
_zt_event_mod::
	.ds 1
_zt_event_arp::
	.ds 1
_zt_event_rate::
	.ds 1
_zt_event_ceil::
	.ds 1
_zt_end_pending::
	.ds 1		; the final row finishes before playback becomes DONE

	.area _CODE

; ---------------------------------------------------------------------------
; void snd_ztr_tick(void);
; ---------------------------------------------------------------------------
_snd_ztr_tick::
	; Only PLAYING runs: an UNDERRUN holds muted until snd_service sees
	; the due row published and resumes (the tick never consumes a row
	; outside PLAYING).
	ld	a, (_snd_ztr_state)
	cp	#SND_ZTR_PLAYING
	ret	nz
_zt_run:
	; Take the output-mode and shadow-valid decision once for this tick.
	ld	a, (_snd_output_mode)
	or	a
	ld	a, #0
	jr	nz, 1$
	ld	a, (_snd_all_valid)
1$:
	ld	(_snd_fast), a
	ld	a, (_snd_ztr_tick_in_row)
	or	a
	jr	nz, _zt_walk
	; the row's first tick: consume the due record
	call	_zt_take_row
	ld	a, (_snd_ztr_state)
	cp	#SND_ZTR_PLAYING
	ret	nz
_zt_walk:
	; The oracle's effect, macro and render operations fused into one
	; channel-order walk.  Effects/macros write nothing, and tone 2 is
	; visited before the coupled noise voice, preserving the trace.
	call	_zt_walk_fused
	; the row's tick advances
	ld	a, (_snd_ztr_tick_in_row)
	inc	a
	ld	(_snd_ztr_tick_in_row), a
	ld	c, a
	ld	a, (_snd_ztr_speed)
	cp	c
	ret	nz
	xor	a
	ld	(_snd_ztr_tick_in_row), a
	ld	a, (_zt_end_pending)
	or	a
	ret	z
	ld	a, #SND_ZTR_DONE
	ld	(_snd_ztr_state), a
	ret

; ---------------------------------------------------------------------------
; The row consumption: the due head record, or an underrun.  HL clobbered.
; ---------------------------------------------------------------------------
_zt_take_row:
	ld	a, (_snd_ztr_head)
	ld	c, a
	ld	a, (_snd_ztr_tail)
	cp	c
	jr	z, _zt_underrun
	ld	l, c
	ld	h, #0
	add	hl, hl
	ld	de, #_snd_ztr_queue_addrs
	add	hl, de
	ld	e, (hl)
	inc	hl
	ld	d, (hl)		; DE = the record
	ex	de, hl
	ld	a, (hl)		; order
	ld	c, a
	ld	a, (_snd_ztr_order)
	cp	c
	jr	nz, _zt_underrun
	inc	hl
	ld	a, (hl)		; row
	ld	c, a
	ld	a, (_snd_ztr_row)
	cp	c
	jr	nz, _zt_underrun
	inc	hl			; the first channel slot
	; the reference's begin_row pre-pass (per-channel effect reset)
	; clobbers HL: keep the slot pointer across it
	push	hl
	call	_zt_begin_row
	pop	hl
	; the events, in channel order
	ld	b, #16
	ld	a, (_snd_ztr_core_page)
	ld	d, a
	ld	e, #0			; DE = channel 0's base
_zt_apply_loop:
	ld	a, (hl)		; the mask; 0 skips the channel
	or	a
	jr	z, _zt_apply_next
	push	bc
	push	de
	push	hl
	call	_zt_apply_event	; HL = the slot, DE = the channel base
	pop	hl
	pop	de
	pop	bc
	ld	a, (_snd_ztr_state)
	cp	#SND_ZTR_PLAYING
	jr	nz, _zt_apply_aborted
_zt_apply_next:
	ld	a, l
	add	a, #SND_ZTR_SLOT_BYTES
	ld	l, a
	jr	nc, 1$
	inc	h			; the record crosses a page boundary
1$:
	push	af
	ld	a, e
	add	a, #16
	ld	e, a
	pop	af
	djnz	_zt_apply_loop
_zt_apply_aborted:
	; the head advances only after the row is applied
	ld	a, (_snd_ztr_head)
	inc	a
	cp	#SND_ZTR_QUEUE_ROWS
	jr	nz, 1$
	xor	a
1$:
	ld	(_snd_ztr_head), a
	ld	a, (_snd_ztr_state)
	cp	#SND_ZTR_PLAYING
	ret	nz
	jp	_zt_advance_position

_zt_underrun:
	ld	a, (_snd_ztr_state)
	cp	#SND_ZTR_UNDERRUN
	ret	z			; already muted and counted
	ld	hl, (_snd_ztr_underruns)
	inc	hl
	ld	(_snd_ztr_underruns), hl
	ld	a, #SND_ZTR_UNDERRUN
	ld	(_snd_ztr_state), a
	jp	_snd_ztr_mute

; ---------------------------------------------------------------------------
; The reference's begin_row: every channel's effect is retired; an
; arpeggio effect re-dirties the pitch.
; ---------------------------------------------------------------------------
_zt_begin_row:
	ld	a, (_snd_ztr_core_page)
	ld	h, a
	ld	l, #0
	ld	b, #16
_zt_br_loop:
	ld	a, l
	or	#ZC_WORK
	ld	l, a
	bit	0, (hl)
	jr	z, _zt_br_next
	ld	a, l
	and	#0xF0
	or	#ZC_EFFECT
	ld	l, a
	ld	c, (hl)		; the effect
	ld	a, c
	cp	#FX_ARPEGGIO
	jr	nz, _zt_br_clear
	push	bc
	push	hl
	ld	a, l
	and	#0xF0
	ld	l, a
	ld	a, #DY_PITCH|DY_NOISE
	call	_zt_dirty
	pop	hl
	pop	bc
_zt_br_clear:
	ld	(hl), #0		; ZC_EFFECT
	inc	h
	ld	a, l
	and	#0xF0
	or	#ZC2_EFFECT_PHASE
	ld	l, a
	ld	(hl), #0
	dec	h
	ld	a, l
	and	#0xF0
	or	#ZC_WORK
	ld	l, a
	res	0, (hl)
	ld	a, l
	and	#0xF0
	ld	l, a
_zt_br_next:
	ld	a, l
	and	#0xF0
	add	a, #16
	ld	l, a
	djnz	_zt_br_loop
	ret

; ---------------------------------------------------------------------------
; One channel's event (the reference's decode_row): HL = the slot,
; DE = the channel base.  Returns HL = the channel base.
; ---------------------------------------------------------------------------
_zt_apply_event:
	push	de
	ld	a, (hl)
	ld	(_zt_event_mask), a
	inc	hl
	ld	a, (hl)		; note
	ld	(_zt_event_note), a
	inc	hl
	ld	a, (hl)		; instrument
	ld	(_zt_event_instr), a
	inc	hl
	ld	a, (hl)		; volume
	ld	(_zt_event_vol), a
	inc	hl
	ld	a, (hl)		; effect
	ld	(_zt_event_effect), a
	inc	hl
	ld	a, (hl)		; parameter
	ld	(_zt_event_param), a
	inc	hl
	ld	a, (hl)		; modifiers
	ld	(_zt_event_mod), a
	inc	hl
	ld	a, (hl)		; arpeggio
	ld	(_zt_event_arp), a
	inc	hl
	ld	a, (hl)		; hairpin rate
	ld	(_zt_event_rate), a
	inc	hl
	ld	a, (hl)		; hairpin ceiling
	ld	(_zt_event_ceil), a
	pop	hl			; HL = the channel base
	ld	a, (_zt_event_mod)
	or	a
	call	nz, _zt_process_modifiers
	ld	a, (_zt_event_instr)
	cp	#0xFF
	jr	z, _zt_ae_volume
	ld	c, a
	ld	a, l
	or	#ZC_INSTRUMENT
	ld	l, a
	ld	(hl), c
	ld	a, l
	and	#0xF0
	ld	l, a
	ld	a, c
	call	_zt_bind_instrument
_zt_ae_volume:
	ld	a, (_zt_event_vol)
	cp	#0xFF
	jr	z, _zt_ae_effect
	ld	c, a
	ld	a, l
	or	#ZC_BASE_VOL
	ld	l, a
	ld	a, (hl)
	cp	c
	jr	z, _zt_ae_effect
	ld	(hl), c
	ld	a, l
	and	#0xF0
	ld	l, a
	ld	a, #DY_VOLUME
	call	_zt_dirty
_zt_ae_effect:
	ld	a, (_zt_event_effect)
	or	a
	call	nz, _zt_process_effect
	ld	a, (_zt_event_mask)
	and	#EV_OFF
	jr	z, _zt_ae_release
	call	_zt_clear_notation
	ld	a, l
	or	#ZC_FLAGS
	ld	l, a
	ld	a, (hl)
	and	#CF_LEGATO
	ld	(hl), a
	ld	a, l
	and	#0xF0
	ld	l, a
	ld	a, #DY_VOLUME
	call	_zt_dirty
	ret
_zt_ae_release:
	ld	a, (_zt_event_mask)
	and	#EV_RELEASE
	jr	z, _zt_ae_note
	ld	a, l
	or	#ZC_FLAGS
	ld	l, a
	set	1, (hl)		; RELEASED
	ld	a, l
	and	#0xF0
	ld	l, a
	jp	_zt_release_macros
_zt_ae_note:
	ld	a, (_zt_event_note)
	cp	#0xFF
	ret	z
	ld	c, a
	ld	a, l
	or	#ZC_FLAGS
	ld	l, a
	ld	a, (hl)
	ld	b, a
	ld	a, (_zt_event_mask)
	and	#EV_LEGATO
	jr	z, _zt_ae_note_on
	ld	a, b
	and	#CF_ACTIVE
	jr	z, _zt_ae_note_on
	; the legato note: pitch update only
	ld	a, l
	and	#0xF0
	or	#ZC_NOTE
	ld	l, a
	ld	(hl), c
	ld	a, l
	and	#0xF0
	ld	l, a
	ld	a, #DY_PITCH|DY_NOISE
	jp	_zt_dirty
_zt_ae_note_on:
	ld	a, c
	jp	_zt_note_on

; ---------------------------------------------------------------------------
; The persistent notation modifiers.  HL = the channel base.
; ---------------------------------------------------------------------------
_zt_process_modifiers:
	ld	a, (_zt_event_mod)
	bit	7, a		; RESET
	jr	z, 1$
	call	_zt_clear_notation
	ld	a, (_zt_event_mod)
1$:
	bit	5, a		; ARP_CLEAR
	jr	z, 2$
	push	af
	ld	a, l
	or	#ZC_FLAGS
	ld	l, a
	res	3, (hl)
	inc	h
	ld	a, l
	and	#0xF0
	or	#ZC2_NOT_PHASE
	ld	l, a
	ld	(hl), #0
	dec	h
	ld	a, l
	and	#0xF0
	ld	l, a
	ld	a, #DY_PITCH|DY_NOISE
	call	_zt_dirty
	pop	af
2$:
	bit	6, a		; ARP_SET
	jr	z, 3$
	push	af
	ld	a, l
	or	#ZC_FLAGS
	ld	l, a
	set	3, (hl)
	inc	h
	ld	a, l
	and	#0xF0
	or	#ZC2_NOT_ARP
	ld	l, a
	ld	a, (_zt_event_arp)
	ld	(hl), a
	ld	a, l
	and	#0xF0
	or	#ZC2_NOT_PHASE
	ld	l, a
	ld	(hl), #2
	dec	h
	ld	a, l
	and	#0xF0
	ld	l, a
	ld	a, #DY_PITCH|DY_NOISE
	call	_zt_dirty
	pop	af
3$:
	bit	3, a		; HAIRPIN_CLEAR
	jr	z, 4$
	ld	a, l
	and	#0xF0
	ld	l, a
	inc	h
	ld	a, l
	or	#ZC2_HAIR_RATE
	ld	l, a
	ld	(hl), #0
	dec	h
	ld	a, l
	and	#0xF0
	ld	l, a
4$:
	bit	4, a		; HAIRPIN_SET
	jr	z, 5$
	ld	a, l
	and	#0xF0
	ld	l, a
	inc	h
	ld	a, l
	or	#ZC2_HAIR_RATE
	ld	l, a
	ld	a, (_zt_event_rate)
	ld	(hl), a
	ld	a, l
	and	#0xF0
	or	#ZC2_HAIR_CEIL
	ld	l, a
	ld	a, (_zt_event_ceil)
	ld	(hl), a
	dec	h
	ld	a, l
	and	#0xF0
	ld	l, a
5$:
	; the notation work bit follows the flags and the hairpin rate
	ld	a, l
	or	#ZC_FLAGS
	ld	l, a
	bit	3, (hl)
	jr	nz, _zt_pm_set
	ld	a, l
	and	#0xF0
	ld	l, a
	inc	h
	ld	a, l
	or	#ZC2_HAIR_RATE
	ld	l, a
	ld	a, (hl)
	or	a
	jr	nz, _zt_pm_set0
	dec	h
	ld	a, l
	and	#0xF0
	or	#ZC_WORK
	ld	l, a
	res	1, (hl)
	ld	a, l
	and	#0xF0
	ld	l, a
	ret
_zt_pm_set0:
	dec	h
_zt_pm_set:
	ld	a, l
	and	#0xF0
	or	#ZC_WORK
	ld	l, a
	set	1, (hl)
	ld	a, l
	and	#0xF0
	ld	l, a
	ret

_zt_clear_notation:
	ld	a, l
	or	#ZC_FLAGS
	ld	l, a
	res	3, (hl)
	inc	h
	ld	a, l
	and	#0xF0
	or	#ZC2_NOT_ARP
	ld	l, a
	xor	a
	ld	(hl), a
	inc	l
	ld	(hl), a		; ZC2_NOT_PHASE
	ld	a, l
	and	#0xF0
	or	#ZC2_HAIR_RATE
	ld	l, a
	xor	a
	ld	(hl), a
	inc	l
	ld	(hl), a		; ZC2_HAIR_OFF
	inc	l
	ld	(hl), a		; ZC2_HAIR_CEIL
	dec	h
	ld	a, l
	and	#0xF0
	or	#ZC_WORK
	ld	l, a
	res	1, (hl)
	ld	a, l
	and	#0xF0
	ld	l, a
	ld	a, #DY_PITCH|DY_VOLUME|DY_NOISE
	jp	_zt_dirty

; ---------------------------------------------------------------------------
; The native effects.  HL = the channel base, A = the effect.
; ---------------------------------------------------------------------------
_zt_process_effect:
	cp	#FX_STOP
	jr	z, _zt_pe_stop
	cp	#FX_LEGATO
	jr	z, _zt_pe_legato
	cp	#FX_VOL_DEC
	jr	z, _zt_pe_dec
	; 1 (ARP), 2 (VOL_SLIDE), 5 (FAST_VOL): arm the effect
	ld	c, a
	ld	a, l
	or	#ZC_EFFECT
	ld	l, a
	ld	(hl), c
	inc	l
	ld	a, (_zt_event_param)
	ld	(hl), a
	inc	h
	ld	a, l
	and	#0xF0
	or	#ZC2_EFFECT_PHASE
	ld	l, a
	ld	(hl), #0
	dec	h
	ld	a, l
	and	#0xF0
	ld	l, a
	ld	a, #DY_PITCH|DY_NOISE
	call	_zt_dirty
	ld	a, l
	or	#ZC_WORK
	ld	l, a
	set	0, (hl)
	ld	a, l
	and	#0xF0
	ld	l, a
	ret
_zt_pe_legato:
	ld	a, (_zt_event_param)
	or	a
	jr	z, 1$
	ld	a, l
	or	#ZC_FLAGS
	ld	l, a
	set	2, (hl)
	ld	a, l
	and	#0xF0
	ld	l, a
	ret
1$:
	ld	a, l
	or	#ZC_FLAGS
	ld	l, a
	res	2, (hl)
	ld	a, l
	and	#0xF0
	ld	l, a
	ret
_zt_pe_dec:
	ld	a, l
	or	#ZC_BASE_VOL
	ld	l, a
	ld	e, (hl)		; the base
	ld	a, (_zt_event_param)
	ld	d, a
	cp	e
	jr	nc, 2$		; parameter >= base: 0
	ld	a, e
	sub	d
	jr	3$
2$:
	xor	a
3$:
	ld	c, a
	ld	a, l
	and	#0xF0
	or	#ZC_BASE_VOL
	ld	l, a
	ld	a, (hl)
	cp	c
	jr	z, _zt_pe_ret
	ld	(hl), c
	ld	a, l
	and	#0xF0
	ld	l, a
	ld	a, #DY_VOLUME
	jp	_zt_dirty
_zt_pe_ret:
	ld	a, l
	and	#0xF0
	ld	l, a
	ret
_zt_pe_stop:
	ld	a, #SND_ZTR_DONE
	ld	(_snd_ztr_state), a
	ret

; ---------------------------------------------------------------------------
; note_on.  HL = the channel base, A = the note.
; ---------------------------------------------------------------------------
_zt_note_on:
	ld	c, a
	ld	a, l
	or	#ZC_FLAGS
	ld	l, a
	ld	a, (hl)
	and	#(CF_ACTIVE|CF_LEGATO)
	cp	#(CF_ACTIVE|CF_LEGATO)
	jr	nz, _zt_no_full
	ld	a, l
	and	#0xF0
	or	#ZC_NOTE
	ld	l, a
	ld	(hl), c
	ld	a, l
	and	#0xF0
	ld	l, a
	ld	a, #DY_PITCH|DY_NOISE
	jp	_zt_dirty
_zt_no_full:
	ld	a, l
	and	#0xF0
	or	#ZC_NOTE
	ld	l, a
	ld	(hl), c
	ld	a, l
	and	#0xF0
	or	#ZC_FLAGS
	ld	l, a
	ld	a, (hl)
	and	#CF_LEGATO
	or	#CF_ACTIVE
	ld	(hl), a
	ld	a, l
	and	#0xF0
	or	#ZC_MACRO_VOL
	ld	l, a
	ld	(hl), #15
	; arpeggio = 0, flags = 0, pitch = 0, duty = 0, phase = 0
	ld	a, l
	and	#0xF0
	or	#ZC_ARP_LO
	ld	l, a
	xor	a
	ld	(hl), a
	ld	a, l
	and	#0xF0
	or	#ZC_ARP_HI
	ld	l, a
	ld	(hl), a
	ld	a, l
	and	#0xF0
	or	#ZC_ARP_FLAGS
	ld	l, a
	ld	(hl), #0
	ld	a, l
	and	#0xF0
	or	#ZC_PITCH_LO
	ld	l, a
	xor	a
	ld	(hl), a
	inc	l
	ld	(hl), a
	ld	a, l
	and	#0xF0
	or	#ZC_DUTY
	ld	l, a
	xor	a
	ld	(hl), a
	ld	a, l
	and	#0xF0
	or	#ZC_PHASE
	ld	l, a
	ld	(hl), a
	call	_zt_reset_macros
	ld	a, l
	and	#0xF0
	ld	l, a
	ld	a, #DY_PITCH|DY_VOLUME|DY_NOISE|DY_PHASE
	jp	_zt_dirty

; ---------------------------------------------------------------------------
; The macro slot walks.  HL = the channel base.
; ---------------------------------------------------------------------------
_zt_macro_base:		; -> HL = the macro slot base for _zt_slot/_zt_chl
	ld	a, (_zt_slot)
	ld	e, a
	ld	d, #0
	ld	hl, #_snd_ztr_macro_page
	add	hl, de
	ld	a, (hl)
	ld	h, a
	ld	a, (_zt_chl)
	ld	l, a
	ret

; reset_macros: every bound slot restarts at its first item.
_zt_reset_macros:
	ld	a, l
	and	#0xF0
	ld	(_zt_chl), a
	ld	l, a
	xor	a
_zt_rm_loop:
	ld	(_zt_slot), a
	push	af
	call	_zt_macro_base
	ld	a, l
	and	#0xF0
	or	#ZM_INIT_DELAY
	ld	l, a
	ld	c, (hl)		; the bound macro's initial delay, or 0
	; Every slot resets position and timer, including an absent macro.
	ld	a, l
	and	#0xF0
	or	#ZM_POS_LO
	ld	l, a
	xor	a
	ld	(hl), a
	inc	l
	ld	(hl), a
	ld	a, l
	and	#0xF0
	or	#ZM_TIMER
	ld	l, a
	ld	(hl), #0
	ld	a, l
	and	#0xF0
	or	#ZM_DELAY
	ld	l, a
	ld	(hl), c
_zt_rm_none:
	pop	af
	inc	a
	cp	#5
	jr	c, _zt_rm_loop
	ld	a, (_snd_ztr_core_page)
	ld	h, a
	ld	a, (_zt_chl)
	ld	l, a
	ret

; release_macros: an active release jumps to its release index.
_zt_release_macros:
	ld	a, l
	and	#0xF0
	ld	(_zt_chl), a
	ld	l, a
	xor	a
_zt_rl_loop:
	ld	(_zt_slot), a
	push	af
	call	_zt_macro_base
	ld	a, l
	and	#0xF0
	or	#ZM_MACRO_LO
	ld	l, a
	ld	e, (hl)
	inc	l
	ld	d, (hl)		; the macro address
	ld	a, e
	or	d
	jr	z, _zt_rl_next
	ld	(_zt_macro_ptr), de
	ex	de, hl			; HL = the macro header
	inc	hl
	inc	hl
	bit	0, (hl)		; the header's release-request flag
	ld	de, (_zt_macro_ptr)	; restore the header, not header + 2
	jr	z, _zt_rl_next
	push	de			; the macro address
	ld	hl, #8
	add	hl, de
	ld	e, (hl)
	inc	hl
	ld	d, (hl)		; the release index
	ld	a, d
	and	e
	inc	a
	jr	z, _zt_rl_none		; FFFFh: no release
	; position = the release index; timer = delay = 0
	push	de
	call	_zt_macro_base
	pop	de
	ld	a, l
	and	#0xF0
	or	#ZM_POS_LO
	ld	l, a
	ld	(hl), e
	inc	l
	ld	(hl), d
	ld	a, l
	and	#0xF0
	or	#ZM_TIMER
	ld	l, a
	ld	(hl), #0
	ld	a, l
	and	#0xF0
	or	#ZM_DELAY
	ld	l, a
	ld	(hl), #0
_zt_rl_none:
	pop	de
_zt_rl_next:
	pop	af
	inc	a
	cp	#5
	jr	c, _zt_rl_loop
	ld	a, (_snd_ztr_core_page)
	ld	h, a
	ld	a, (_zt_chl)
	ld	l, a
	ret

; bind_instrument: the resident entry's five macro offsets, rebound.
; HL = the channel base, A = the instrument number.
_zt_bind_instrument:
	ld	(_zt_instr_num), a
	ld	a, l
	and	#0xF0
	ld	(_zt_chl), a
	ld	a, (_zt_instr_num)
	rlca
	rlca
	rlca
	rlca
	and	#0xF0
	ld	(_zt_entry_l), a
	xor	a
_zt_bi_loop:
	ld	(_zt_slot), a
	push	af
	; the entry's slot offset: entry + 4 + slot * 2
	ld	b, a
	ld	a, (_zt_entry_l)
	or	#0x04
	ld	l, a
	ld	a, b
	add	a, a
	add	a, l
	ld	l, a
	ld	a, (_snd_ztr_instr_page)
	ld	h, a
	ld	e, (hl)
	inc	l
	ld	d, (hl)		; DE = the resident offset
	ld	a, e
	or	d
	jr	z, _zt_bi_none
	call	_zt_bind_one
	jr	_zt_bi_next
_zt_bi_none:
	call	_zt_bind_none
_zt_bi_next:
	pop	af
	inc	a
	cp	#5
	jr	c, _zt_bi_loop
	ld	a, (_snd_ztr_core_page)
	ld	h, a
	ld	a, (_zt_chl)
	ld	l, a
	ret

; DE = the macro's resident offset.  Binds the slot's literal fields.
_zt_bind_one:
	ld	hl, (_snd_ztr_macro_base)
	add	hl, de			; HL = the macro header
	ld	(_zt_head), hl
	call	_zt_macro_base
	ld	a, h
	ld	(_zt_sloth), a
	ld	a, l
	and	#0xF0
	or	#ZM_MACRO_LO
	ld	l, a
	ld	de, (_zt_head)
	ld	(hl), e		; the macro address
	inc	l
	ld	(hl), d
	; Cache the header bounds without changing macro_position's index
	; semantics.  The due-step path reads these from the slot page.
	ld	hl, (_zt_head)
	ld	bc, #6
	add	hl, bc
	ld	e, (hl)
	inc	hl
	ld	d, (hl)
	ld	(_zt_loop), de
	inc	hl
	ld	e, (hl)
	inc	hl
	ld	d, (hl)
	ld	(_zt_release), de
	inc	hl
	ld	e, (hl)
	inc	hl
	ld	d, (hl)
	ld	(_zt_count), de
	ld	a, (_zt_sloth)
	ld	h, a
	ld	a, (_zt_chl)
	ld	l, a
	ld	a, l
	and	#0xF0
	or	#ZM_LOOP_LO
	ld	l, a
	ld	de, (_zt_loop)
	ld	(hl), e
	inc	l
	ld	(hl), d
	inc	l
	ld	de, (_zt_release)
	ld	(hl), e
	inc	l
	ld	(hl), d
	inc	l
	ld	de, (_zt_count)
	ld	(hl), e
	inc	l
	ld	(hl), d
	; Header delay byte used by every full note-on reset.
	ld	hl, (_zt_head)
	ld	bc, #4
	add	hl, bc
	ld	a, (hl)
	ld	(_zt_delay), a
	ld	a, (_zt_sloth)
	ld	h, a
	ld	a, (_zt_chl)
	or	#ZM_INIT_DELAY
	ld	l, a
	ld	a, (_zt_delay)
	ld	(hl), a
	; the slot's work bit (2 + slot)
	ld	a, (_snd_ztr_core_page)
	ld	h, a
	ld	a, (_zt_chl)
	or	#ZC_WORK
	ld	l, a
	ld	a, (_zt_slot)
	ld	b, a
	inc	b
	ld	a, #0x02
_zt_bi_shift:
	add	a, a
	djnz	_zt_bi_shift
	or	(hl)
	ld	(hl), a
	ret

_zt_bind_none:		; no macro: an absent slot
	call	_zt_macro_base
	ld	a, l
	and	#0xF0
	or	#ZM_MACRO_LO
	ld	l, a
	xor	a
	ld	b, #2			; the null pointer; cached bounds are ignored
_zt_bn_zero:
	ld	(hl), a
	inc	l
	djnz	_zt_bn_zero
	ld	a, l
	and	#0xF0
	or	#ZM_INIT_DELAY
	ld	l, a
	ld	(hl), #0
	ld	a, (_snd_ztr_core_page)
	ld	h, a
	ld	a, (_zt_chl)
	or	#ZC_WORK
	ld	l, a
	ld	a, (_zt_slot)
	ld	b, a
	inc	b
	ld	a, #0x02
_zt_bn_shift:
	add	a, a
	djnz	_zt_bn_shift
	cpl
	and	(hl)
	ld	(hl), a
	ret
; ---------------------------------------------------------------------------
; bound_macro_position, on indexes: DE = a position -> DE = the bounded
; position.  Matches the reference exactly, released state included.
; ---------------------------------------------------------------------------
_zt_bound_position:
	ld	(_zt_pos), de
	; The literal header bounds are cached in the slot record at bind.
	ld	a, (_zt_chl)
	or	#ZM_LOOP_LO
	ld	l, a
	ld	a, (_zt_sloth)
	ld	h, a
	ld	e, (hl)
	inc	l
	ld	d, (hl)
	ld	(_zt_loop), de
	inc	l
	ld	e, (hl)
	inc	hl
	ld	d, (hl)
	ld	(_zt_release), de
	inc	l
	ld	e, (hl)
	inc	hl
	ld	d, (hl)
	ld	(_zt_count), de
	; released?
	ld	a, (_snd_ztr_core_page)
	ld	h, a
	ld	a, (_zt_chl)
	or	#ZC_FLAGS
	ld	l, a
	bit	1, (hl)		; RELEASED
	jr	nz, _zt_bp_count
	; not released: a release index in range rebinds the position
	ld	hl, (_zt_release)
	ld	a, h
	and	l
	inc	a
	jr	z, _zt_bp_count		; FFFFh: no release
	ld	de, (_zt_pos)
	ex	de, hl
	or	a
	sbc	hl, de			; position - release
	jr	c, _zt_bp_count		; position < release
	ld	hl, (_zt_loop)
	ld	a, h
	and	l
	inc	a
	jr	z, 1$
	ld	de, (_zt_loop)
	ret
1$:
	ld	de, (_zt_release)
	ld	a, e
	or	d
	ret	z			; release 0: 0
	dec	de
	ret
_zt_bp_count:
	; position >= count?
	ld	hl, (_zt_count)
	ld	de, (_zt_pos)
	ex	de, hl
	or	a
	sbc	hl, de			; position - count
	jr	c, _zt_bp_keep
	ld	hl, (_zt_release)
	ld	a, h
	and	l
	inc	a
	jr	nz, 2$
	ld	hl, (_zt_loop)
	ld	a, h
	and	l
	inc	a
	jr	z, 2$
	ld	de, (_zt_loop)
	ret
2$:
	ld	de, (_zt_count)
	dec	de
	ret
_zt_bp_keep:
	ld	de, (_zt_pos)
	ret
; ---------------------------------------------------------------------------
; ---------------------------------------------------------------------------
; advance_macro, the reference's port.  HL = the slot base.  Sets
; _zt_stepped and, when stepped, _zt_vflags and _zt_value.
; ---------------------------------------------------------------------------
_zt_advance_macro:
	xor	a
	ld	(_zt_stepped), a
	ld	a, h
	ld	(_zt_sloth), a
	ld	a, l
	and	#0xF0
	ld	(_zt_chl), a
	or	#ZM_MACRO_LO
	ld	l, a
	ld	e, (hl)
	inc	l
	ld	d, (hl)		; the macro address
	ld	a, e
	or	d
	ret	z			; no macro
	ld	(_zt_macro_ptr), de
	ld	a, l
	and	#0xF0
	or	#ZM_DELAY
	ld	l, a
	ld	a, (hl)
	or	a
	jr	z, _zt_am_timer
	dec	(hl)
	ret
_zt_am_timer:
	ld	a, l
	and	#0xF0
	or	#ZM_TIMER
	ld	l, a
	ld	a, (hl)
	or	a
	jr	z, _zt_am_due
	dec	(hl)
	ret
_zt_am_due:
	; DE = bound(position)
	ld	a, l
	and	#0xF0
	or	#ZM_POS_LO
	ld	l, a
	ld	e, (hl)
	inc	l
	ld	d, (hl)
	ld	a, l
	and	#0xF0
	ld	l, a
	ld	(_zt_pos), de
	; the item: macro + 12 + position * 3
	ex	de, hl
	ld	e, l
	ld	d, h			; DE = the position
	add	hl, hl
	add	hl, de			; position * 3
	ld	de, #12
	add	hl, de
	ld	de, (_zt_macro_ptr)
	add	hl, de			; HL = the item
	ld	a, (hl)		; the value flags
	ld	(_zt_vflags), a
	inc	hl
	ld	e, (hl)
	inc	hl
	ld	d, (hl)		; the value
	ld	(_zt_value), de
	; the timer: step ? step - 1 : 0
	ld	hl, (_zt_macro_ptr)
	inc	hl
	inc	hl
	inc	hl
	ld	a, (hl)		; the step (+3)
	or	a
	jr	z, 1$
	dec	a
1$:
	ld	b, a
	ld	a, (_zt_sloth)
	ld	h, a
	ld	a, (_zt_chl)
	or	#ZM_TIMER
	ld	l, a
	ld	(hl), b
	; the position: bound(position + 1)
	ld	de, (_zt_pos)
	inc	de
	ld	a, l
	and	#0xF0
	ld	l, a
	call	_zt_bound_position
	ld	a, (_zt_sloth)
	ld	h, a			; the bound read left H on the core page
	ld	a, (_zt_chl)
	or	#ZM_POS_LO
	ld	l, a
	ld	(hl), e
	inc	l
	ld	(hl), d
	ld	a, #1
	ld	(_zt_stepped), a
	ret
; ---------------------------------------------------------------------------
; One slot, stepped and applied.  HL = the channel base, A = the slot.
; ---------------------------------------------------------------------------
_zt_step_slot:
	ld	(_zt_slot), a
	ld	a, l
	and	#0xF0
	ld	(_zt_chl), a
	ld	l, a
	call	_zt_macro_base
	call	_zt_advance_macro
	ld	a, (_snd_ztr_core_page)
	ld	h, a
	ld	a, (_zt_chl)
	ld	l, a		; HL = the channel base
	ld	a, (_zt_stepped)
	or	a
	ret	z
	ld	a, (_zt_slot)
	cp	#0
	jp	z, _zt_apply_vol
	cp	#1
	jp	z, _zt_apply_pitch
	cp	#2
	jp	z, _zt_apply_arp
	cp	#3
	jp	z, _zt_apply_duty
	jp	_zt_apply_phase

_zt_apply_vol:		; volume = clamp(value, 0, 15)
	ld	de, (_zt_value)
	ld	a, d
	or	a
	jr	z, 1$
	jp	m, 2$		; negative: 0
	ld	a, #15
	jr	3$
2$:
	xor	a
	jr	3$
1$:
	ld	a, e
	cp	#16
	jr	c, 3$
	ld	a, #15
3$:
	ld	c, a
	ld	a, l
	or	#ZC_MACRO_VOL
	ld	l, a
	ld	a, (hl)
	cp	c
	jp	z, _zt_apply_done
	ld	(hl), c
	ld	a, l
	and	#0xF0
	ld	l, a
	ld	a, #DY_VOLUME
	jp	_zt_dirty

_zt_apply_pitch:
	ld	de, (_zt_value)
	ld	a, l
	or	#ZC_PITCH_LO
	ld	l, a
	ld	a, (hl)
	cp	e
	jr	nz, 1$
	inc	l
	ld	a, (hl)
	cp	d
	jp	z, _zt_apply_done
	dec	l
1$:
	ld	(hl), e
	inc	l
	ld	(hl), d
	ld	a, l
	and	#0xF0
	ld	l, a
	ld	a, #DY_PITCH
	jp	_zt_dirty

_zt_apply_arp:
	ld	de, (_zt_value)
	ld	a, (_zt_vflags)
	and	#0x01
	ld	c, a
	ld	a, l
	or	#ZC_ARP_LO
	ld	l, a
	ld	a, (hl)
	cp	e
	jr	nz, 1$
	ld	a, l
	and	#0xF0
	or	#ZC_ARP_HI
	ld	l, a
	ld	a, (hl)
	cp	d
	jr	nz, 1$
	ld	a, l
	and	#0xF0
	or	#ZC_ARP_FLAGS
	ld	l, a
	ld	a, (hl)
	cp	c
	jr	z, _zt_apply_done
1$:
	ld	a, l
	and	#0xF0
	or	#ZC_ARP_LO
	ld	l, a
	ld	(hl), e		; ZC_ARP_LO
	ld	a, l
	and	#0xF0
	or	#ZC_ARP_HI
	ld	l, a
	ld	(hl), d
	ld	a, l
	and	#0xF0
	or	#ZC_ARP_FLAGS
	ld	l, a
	ld	(hl), c
	ld	a, l
	and	#0xF0
	ld	l, a
	ld	a, #DY_PITCH|DY_NOISE
	jp	_zt_dirty

_zt_apply_duty:
	ld	de, (_zt_value)
	ld	a, e
	and	#0x01
	ld	c, a
	ld	a, l
	or	#ZC_DUTY
	ld	l, a
	ld	a, (hl)
	cp	c
	jr	z, _zt_apply_done
	ld	(hl), c
	ld	a, l
	and	#0xF0
	ld	l, a
	ld	a, #DY_NOISE
	jp	_zt_dirty

_zt_apply_phase:
	ld	de, (_zt_value)
	ld	a, e
	ld	c, a
	ld	a, l
	or	#ZC_PHASE
	ld	l, a
	ld	(hl), c
	ld	a, c
	or	a
	jr	z, _zt_apply_done
	ld	a, l
	and	#0xF0
	ld	l, a
	rrca
	rrca
	rrca
	rrca
	and	#0x03
	cp	#0x03
	jr	nz, _zt_apply_done
	ld	a, #DY_PHASE
	jp	_zt_dirty
_zt_apply_done:
	ld	a, l
	and	#0xF0
	ld	l, a
	ret

; ---------------------------------------------------------------------------
; ---------------------------------------------------------------------------
; The reference's effect/notation, macro and render work, fused into one
; channel-order walk.  Slot order and render order stay literal.
; ---------------------------------------------------------------------------
_zt_walk_fused:
	ld	a, (_snd_ztr_core_page)
	ld	h, a
	ld	l, #0
	ld	b, #16
_zt_fw_chan:
	push	bc
	; Effect and persistent notation.
	ld	a, l
	or	#ZC_WORK
	ld	l, a
	ld	c, (hl)
	ld	a, c
	or	a
	jp	z, _zt_fw_next
	ld	a, c
	and	#0x03
	jr	z, _zt_fw_macros
	ld	a, l
	and	#0xF0
	ld	l, a
	bit	0, c
	jr	z, _zt_fw_notation
	ld	a, (_snd_ztr_tick_in_row)
	or	a
	jr	z, _zt_fw_notation
	call	_zt_advance_effect
_zt_fw_notation:
	ld	a, l
	and	#0xF0
	or	#ZC_WORK
	ld	l, a
	bit	1, (hl)
	jr	z, _zt_fw_macros
	ld	a, l
	and	#0xF0
	ld	l, a
	call	_zt_advance_notation
_zt_fw_macros:
	; Active channel macros, slots 0..4.
	ld	a, l
	and	#0xF0
	ld	l, a
	ld	a, l
	or	#ZC_FLAGS
	ld	l, a
	bit	0, (hl)		; ACTIVE
	jr	z, _zt_fw_render
	ld	a, l
	and	#0xF0
	or	#ZC_WORK
	ld	l, a
	ld	a, (hl)
	and	#0x7C			; the five bound macro slots
	jr	z, _zt_fw_render
	ld	a, l
	and	#0xF0
	ld	l, a
	ld	a, l
	or	#ZC_WORK
	ld	l, a
	bit	2, (hl)
	jr	z, 1$
	ld	a, l
	and	#0xF0
	ld	l, a
	xor	a
	call	_zt_step_slot
1$:
	ld	a, l
	or	#ZC_WORK
	ld	l, a
	bit	3, (hl)
	jr	z, 2$
	ld	a, l
	and	#0xF0
	ld	l, a
	ld	a, #1
	call	_zt_step_slot
2$:
	ld	a, l
	or	#ZC_WORK
	ld	l, a
	bit	4, (hl)
	jr	z, 3$
	ld	a, l
	and	#0xF0
	ld	l, a
	ld	a, #2
	call	_zt_step_slot
3$:
	ld	a, l
	or	#ZC_WORK
	ld	l, a
	bit	5, (hl)
	jr	z, 4$
	ld	a, l
	and	#0xF0
	ld	l, a
	ld	a, #3
	call	_zt_step_slot
4$:
	ld	a, l
	or	#ZC_WORK
	ld	l, a
	bit	6, (hl)
	jr	z, _zt_fw_render
	ld	a, l
	and	#0xF0
	ld	l, a
	ld	a, #4
	call	_zt_step_slot
_zt_fw_render:
	; Dirty render, still in channel order.
	ld	a, l
	and	#0xF0
	ld	l, a
	ld	a, l
	or	#ZC_WORK
	ld	l, a
	bit	7, (hl)
	jr	z, _zt_fw_next
	ld	a, l
	and	#0xF0
	ld	l, a
	call	_zt_render
_zt_fw_next:
	pop	bc
	ld	a, l
	and	#0xF0
	add	a, #16
	ld	l, a
	dec	b
	jp	nz, _zt_fw_chan
	ret

; The effect advance (the reference's advance_effects).
_zt_advance_effect:
	ld	a, l
	or	#ZC_EFFECT
	ld	l, a
	ld	c, (hl)		; the effect
	inc	l
	ld	b, (hl)		; the parameter
	ld	a, c
	cp	#FX_ARPEGGIO
	jr	z, _zt_ze_arp
	cp	#FX_VOL_SLIDE
	jr	z, _zt_ze_vol
	cp	#FX_FAST_VOL
	jr	z, _zt_ze_vol
	ld	a, l
	and	#0xF0
	ld	l, a
	ret			; LEGATO, VOL_DEC and STOP have no advance
_zt_ze_arp:
	ld	a, l
	and	#0xF0
	ld	l, a
	inc	h
	ld	a, l
	or	#ZC2_EFFECT_PHASE
	ld	l, a
	ld	a, (hl)
	inc	a
	cp	#3
	jr	nz, 1$
	xor	a
1$:
	ld	(hl), a
	dec	h
	ld	a, l
	and	#0xF0
	ld	l, a
	ld	a, #DY_PITCH|DY_NOISE
	jp	_zt_dirty
_zt_ze_vol:			; B = the parameter, C = the effect
	ld	a, b
	ld	e, a
	and	#0xF0
	jr	nz, _zt_ze_up
	; down: amount = parameter & 0Fh (x4 when fast)
	ld	a, e
	and	#0x0F
	ld	d, a
	ld	a, c
	cp	#FX_FAST_VOL
	jr	nz, 2$
	ld	a, d
	add	a, a
	add	a, a
	ld	d, a
2$:
	ld	a, l
	and	#0xF0
	or	#ZC_BASE_VOL
	ld	l, a
	ld	a, (hl)		; the base
	ld	e, a
	ld	a, d
	cp	e
	jr	nc, 3$		; amount >= base: 0
	ld	a, e
	sub	d
	jr	_zt_ze_store
3$:
	xor	a
	jr	_zt_ze_store
_zt_ze_up:
	ld	a, e
	rrca
	rrca
	rrca
	rrca
	and	#0x0F		; amount = parameter >> 4
	ld	d, a
	ld	a, c
	cp	#FX_FAST_VOL
	jr	nz, 4$
	ld	a, d
	add	a, a
	add	a, a
	ld	d, a
4$:
	ld	a, l
	and	#0xF0
	or	#ZC_BASE_VOL
	ld	l, a
	ld	a, (hl)
	add	a, d
	jr	c, 5$
	cp	#16
	jr	c, _zt_ze_store
5$:
	ld	a, #15
_zt_ze_store:
	ld	c, a
	ld	a, l
	and	#0xF0
	or	#ZC_BASE_VOL
	ld	l, a
	ld	a, (hl)
	cp	c
	jr	z, _zt_ze_ret
	ld	(hl), c
	ld	a, l
	and	#0xF0
	ld	l, a
	ld	a, #DY_VOLUME
	jp	_zt_dirty
_zt_ze_ret:
	ld	a, l
	and	#0xF0
	ld	l, a
	ret

; The notation advance (the reference's advance_notation).
_zt_advance_notation:
	ld	a, l
	or	#ZC_FLAGS
	ld	l, a
	bit	3, (hl)		; NOT_ARP
	jr	z, _zt_zn_hair
	inc	h
	ld	a, l
	and	#0xF0
	or	#ZC2_NOT_PHASE
	ld	l, a
	ld	a, (hl)
	inc	a
	cp	#3
	jr	nz, 1$
	xor	a
1$:
	ld	(hl), a
	dec	h
	ld	a, l
	and	#0xF0
	ld	l, a
	ld	a, #DY_PITCH|DY_NOISE
	call	_zt_dirty
_zt_zn_hair:
	inc	h
	ld	a, l
	and	#0xF0
	or	#ZC2_HAIR_RATE
	ld	l, a
	ld	b, (hl)		; the rate
	ld	a, b
	or	a
	jr	z, _zt_zn_done
	ld	a, l
	and	#0xF0
	or	#ZC2_HAIR_OFF
	ld	l, a
	ld	a, (hl)
	add	a, b			; the next offset
	ld	c, a
	or	a
	jp	p, 2$			; positive
	cp	#0xF1			; -15
	jr	nc, 3$			; >= -15: keep
	ld	a, #0xF1
	jr	3$
2$:
	cp	#16
	jr	c, 3$
	ld	a, #15
3$:
	ld	d, a
	ld	a, l
	and	#0xF0
	or	#ZC2_HAIR_OFF
	ld	l, a
	ld	a, (hl)
	cp	d
	jr	z, _zt_zn_done
	ld	(hl), d
	dec	h
	ld	a, l
	and	#0xF0
	ld	l, a
	ld	a, #DY_VOLUME
	jp	_zt_dirty
_zt_zn_done:
	dec	h
	ld	a, l
	and	#0xF0
	ld	l, a
	ret

; ---------------------------------------------------------------------------
; ---------------------------------------------------------------------------
; The render, the reference's psg_render_all for one channel: the
; tone or the noise, then the volume, committed through the library's
; snd_put_* primitives.  HL = the channel base.
; ---------------------------------------------------------------------------
_zt_render:
	; an inactive channel drops the pitch/noise/phase bits
	ld	a, l
	or	#ZC_FLAGS
	ld	l, a
	bit	0, (hl)
	jr	nz, _zt_zr_kind
	ld	a, l
	and	#0xF0
	or	#ZC_DIRTY
	ld	l, a
	ld	a, (hl)
	and	#DY_VOLUME
	ld	(hl), a
	ld	a, l
	and	#0xF0
	ld	l, a
_zt_zr_kind:
	ld	a, l
	and	#0xF0
	or	#ZC_DIRTY
	ld	l, a
	ld	c, (hl)		; C = the dirty bits
	ld	a, l
	and	#0xF0
	ld	l, a
	; the channel kind: voices 3, 7, 11, 15 are noise
	ld	a, l
	rrca
	rrca
	rrca
	rrca
	and	#0x03
	cp	#0x03
	jr	z, _zt_zr_noise
	; a tone channel
	ld	a, c
	and	#DY_PITCH
	jr	z, _zt_zr_volume
	push	bc
	call	_zt_render_tone
	pop	bc
	jr	_zt_zr_volume
_zt_zr_noise:
	ld	a, c
	and	#(DY_PITCH|DY_NOISE|DY_PHASE)
	jr	z, _zt_zr_volume
	push	bc
	call	_zt_render_noise
	pop	bc
_zt_zr_volume:
	ld	a, c
	and	#DY_VOLUME
	jr	z, _zt_zr_end
	call	_zt_render_volume
_zt_zr_end:
	ld	a, l
	and	#0xF0
	ld	l, a
	inc	l			; ZC_DIRTY
	ld	a, (hl)
	or	a
	jr	nz, 1$
	dec	l			; ZC_WORK
	res	7, (hl)
1$:
	ld	a, l
	and	#0xF0
	ld	l, a
	ret

; The tone: the effective note's divisor, the fine pitch, the period
; commit against last_period.  HL = the channel base.
_zt_render_tone:
	call	_zt_effective_note
	push	hl
	ld	l, a
	ld	a, (_snd_ztr_div_lo_page)
	ld	h, a
	ld	e, (hl)
	inc	h
	ld	d, (hl)		; DE = the divisor
	pop	hl			; the channel base
	call	_zt_fine_pitch	; DE = the period
	; the reference's compare: write only on a changed period
	ld	a, l
	and	#0xF0
	ld	l, a
	inc	h
	ld	a, l
	or	#ZC2_LAST_PER_LO
	ld	l, a
	ld	a, (hl)
	cp	e
	jr	nz, _zt_rt_write
	inc	l
	ld	a, (hl)
	cp	d
	jr	nz, _zt_rt_write_back
	dec	h			; the core page
	jr	_zt_rt_clear

_zt_rt_write_back:
	dec	l
_zt_rt_write:
	ld	a, l
	and	#0xF0
	or	#ZC2_LAST_PER_LO
	ld	l, a
	ld	(hl), e
	inc	l
	ld	(hl), d
	dec	h			; the core page
	ld	a, l
	and	#0xF0
	ld	l, a
	push	hl
	ld	a, (_snd_desc_page)
	ld	h, a			; the descriptor base
	ld	a, (_snd_fast)
	or	a
	jr	z, _zt_rt_slow
	; DIRECT with a valid shadow: desired and shadow through the
	; descriptor's precomputed pointers, then the two port bytes.
	ld	a, l
	or	#VD_DESIRED_REG
	ld	l, a
	ld	c, (hl)
	inc	l
	ld	b, (hl)
	ld	a, e
	ld	(bc), a
	inc	bc
	ld	a, d
	ld	(bc), a
	inc	l			; VD_SHADOW_REG
	ld	c, (hl)
	inc	l
	ld	b, (hl)
	ld	a, (bc)
	cp	e
	jr	nz, _zt_rt_fast_write
	inc	bc
	ld	a, (bc)
	cp	d
	jr	z, _zt_rt_commit_done
	dec	bc
_zt_rt_fast_write:
	ld	a, e
	ld	(bc), a
	inc	bc
	ld	a, d
	ld	(bc), a
	ld	a, l
	and	#0xF0
	or	#VD_PORT
	ld	l, a
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
	jr	_zt_rt_commit_done
_zt_rt_slow:
	call	snd_put_period
_zt_rt_commit_done:
	pop	hl			; the channel base
	jr	_zt_rt_clear
_zt_rt_clear:
	ld	a, l
	and	#0xF0
	or	#ZC_DIRTY
	ld	l, a
	res	0, (hl)		; DY_PITCH done
	ld	a, l
	and	#0xF0
	ld	l, a
	ret

; The noise: the effective note's rate and the duty, forced on a phase
; reset.  HL = the channel base.
_zt_render_noise:
	call	_zt_effective_note
	and	#0x03
	ld	c, a			; C = the rate
	ld	a, l
	and	#0xF0
	or	#ZC_DUTY
	ld	l, a
	ld	a, (hl)
	and	#0x01
	rlca
	rlca
	or	c
	or	#0xE0			; A = the control byte
	ld	c, a
	ld	a, l
	and	#0xF0
	or	#ZC_DIRTY
	ld	l, a
	ld	b, (hl)
	ld	a, b
	and	#DY_PHASE
	ld	b, a			; B != 0 forces the write
	; the reference's compare: write on a changed control or a phase
	ld	a, l
	and	#0xF0
	ld	l, a
	inc	h
	ld	a, l
	or	#ZC2_LAST_NOISE
	ld	l, a
	ld	a, (hl)
	cp	c
	jr	nz, _zt_rn_write
	ld	a, b
	or	a
	jr	nz, _zt_rn_write
	dec	h			; the core page
	jr	_zt_rn_clear
_zt_rn_write:
	ld	a, l
	and	#0xF0
	or	#ZC2_LAST_NOISE
	ld	l, a
	ld	(hl), c
	dec	h
	ld	a, l
	and	#0xF0
	ld	l, a
	push	hl
	ld	a, (_snd_desc_page)
	ld	h, a			; the descriptor base
	ld	a, (_snd_fast)
	or	a
	jr	z, _zt_rn_slow
	; DIRECT with a valid shadow.  Preserve force/control in DE while
	; BC follows the descriptor's desired and shadow pointers.
	push	bc
	pop	de			; D = force, E = control
	ld	a, l
	or	#VD_DESIRED_REG
	ld	l, a
	ld	c, (hl)
	inc	l
	ld	b, (hl)
	ld	a, e
	ld	(bc), a
	inc	l			; VD_SHADOW_REG
	ld	c, (hl)
	inc	l
	ld	b, (hl)
	ld	a, (bc)
	cp	e
	jr	nz, _zt_rn_fast_write
	ld	a, d
	or	a
	jr	z, _zt_rn_commit_done
_zt_rn_fast_write:
	ld	a, e
	ld	(bc), a
	ld	a, l
	and	#0xF0
	or	#VD_PORT
	ld	l, a
	ld	c, (hl)
	ld	a, e
	out	(c), a
	jr	_zt_rn_commit_done
_zt_rn_slow:
	ld	a, c
	call	snd_put_noise
_zt_rn_commit_done:
	pop	hl			; the channel base
_zt_rn_clear:
	ld	a, l
	and	#0xF0
	or	#ZC_DIRTY
	ld	l, a
	ld	a, (hl)
	and	#DY_VOLUME
	ld	(hl), a		; DY_PITCH|DY_NOISE|DY_PHASE done
	ld	a, l
	and	#0xF0
	ld	l, a
	ret

; The volume: the loudness through the table, the hairpin, the
; attenuation commit against last_attenuation.  HL = the channel base.
_zt_render_volume:
	push	hl		; the channel base survives the table lookup
	xor	a
	ld	e, a			; E = the loudness
	ld	a, l
	or	#ZC_FLAGS
	ld	l, a
	bit	0, (hl)		; active
	jr	z, _zt_zv_inactive
	ld	a, l
	and	#0xF0
	or	#ZC_BASE_VOL
	ld	l, a
	ld	b, (hl)
	ld	a, l
	and	#0xF0
	or	#ZC_MACRO_VOL
	ld	l, a
	ld	c, (hl)
	ld	a, b
	rrca
	rrca
	rrca
	rrca
	and	#0xF0
	or	c
	ld	l, a
	ld	a, (_snd_ztr_loud_page)
	ld	h, a
	ld	e, (hl)		; the loudness
_zt_zv_inactive:
	pop	hl			; HL = the channel base again
_zt_zv_adj:
	inc	h
	ld	a, l
	and	#0xF0
	or	#ZC2_HAIR_OFF
	ld	l, a
	ld	d, (hl)		; the offset (signed)
	ld	a, e
	add	a, d
	ld	c, a			; the adjusted loudness
	ld	a, d
	or	a
	jp	p, _zt_zv_clamp	; offset <= 0: no ceiling
	ld	a, l
	and	#0xF0
	or	#ZC2_HAIR_CEIL
	ld	l, a
	ld	a, (hl)
	cp	c
	jr	nc, _zt_zv_clamp	; ceiling >= adjusted: keep
	ld	c, a			; the ceiling
_zt_zv_clamp:
	ld	a, c
	or	a
	jp	p, 1$
	xor	a
	ld	c, a
	jr	2$
1$:
	ld	a, c
	cp	#16
	jr	c, 2$
	ld	a, #15
	ld	c, a
2$:
	ld	a, #15
	sub	c			; the attenuation
	ld	c, a
	; the reference's compare: write only on a changed attenuation
	ld	a, l
	and	#0xF0
	or	#ZC2_LAST_ATT
	ld	l, a
	ld	a, (hl)
	cp	c
	jr	nz, _zt_zv_write
	dec	h			; the core page
	jr	_zt_zv_clear
_zt_zv_write:
	ld	(hl), c
	dec	h			; the core page
	ld	a, l
	and	#0xF0
	ld	l, a
	push	hl
	ld	a, (_snd_desc_page)
	ld	h, a			; the descriptor base
	ld	a, (_snd_fast)
	or	a
	jr	z, _zt_zv_slow
	; DIRECT with a valid shadow: the required descriptor walk.
	ld	a, l
	or	#VD_ATT_DESIRED
	ld	l, a
	ld	a, c
	ld	(hl), a
	inc	l
	cp	(hl)
	jr	z, _zt_zv_commit_done
	ld	(hl), a
	inc	l
	or	(hl)
	inc	l
	ld	c, (hl)
	out	(c), a
	jr	_zt_zv_commit_done
_zt_zv_slow:
	ld	a, c
	call	snd_put_atten
_zt_zv_commit_done:
	pop	hl			; the channel base
	jr	_zt_zv_clear
_zt_zv_clear:
	ld	a, l
	and	#0xF0
	or	#ZC_DIRTY
	ld	l, a
	res	1, (hl)		; DY_VOLUME done
	ld	a, l
	and	#0xF0
	ld	l, a
	ret
; ---------------------------------------------------------------------------
; effective_note, the reference's port: A = the note (0..95).
; HL = the channel base.
; ---------------------------------------------------------------------------
_zt_effective_note:
	push	hl
	ld	a, l
	or	#ZC_ARP_FLAGS
	ld	l, a
	ld	b, (hl)
	ld	a, l
	and	#0xF0
	or	#ZC_ARP_LO
	ld	l, a
	ld	c, (hl)
	ld	a, l
	and	#0xF0
	ld	l, a
	bit	0, b
	jr	z, _zt_en_rel
	ld	a, c			; the fixed note
	jr	_zt_en_clamp0
_zt_en_rel:
	ld	a, l
	or	#ZC_NOTE
	ld	l, a
	ld	a, (hl)
	add	a, c
_zt_en_clamp0:
	ld	e, a
	or	a
	jp	p, _zt_en_effect
	xor	a
	ld	e, a
_zt_en_effect:
	ld	a, l
	and	#0xF0
	or	#ZC_EFFECT
	ld	l, a
	ld	b, (hl)		; the effect
	ld	c, e			; C = the note
	ld	a, b
	cp	#FX_ARPEGGIO
	jr	nz, _zt_en_notation
	inc	h
	ld	a, l
	and	#0xF0
	or	#ZC2_EFFECT_PHASE
	ld	l, a
	ld	a, (hl)
	dec	a
	jr	z, _zt_en_effect_x
	dec	a
	jr	z, _zt_en_effect_y
	jr	_zt_en_effect_done
_zt_en_effect_x:
	dec	h
	ld	a, l
	and	#0xF0
	or	#ZC_EFFECT_PARAM
	ld	l, a
	ld	a, (hl)
	rrca
	rrca
	rrca
	rrca
	and	#0x0F
	add	a, c
	ld	c, a
	jr	_zt_en_notation
_zt_en_effect_y:
	dec	h
	ld	a, l
	and	#0xF0
	or	#ZC_EFFECT_PARAM
	ld	l, a
	ld	a, (hl)
	and	#0x0F
	add	a, c
	ld	c, a
	jr	_zt_en_notation
_zt_en_effect_done:
	dec	h
_zt_en_notation:
	ld	a, l
	and	#0xF0
	or	#ZC_FLAGS
	ld	l, a
	bit	3, (hl)		; NOT_ARP
	jr	z, _zt_en_final
	inc	h
	ld	a, l
	and	#0xF0
	or	#ZC2_NOT_PHASE
	ld	l, a
	ld	a, (hl)
	dec	a
	jr	z, _zt_en_not_x
	dec	a
	jr	z, _zt_en_not_y
	jr	_zt_en_not_done
_zt_en_not_x:
	dec	h
	ld	a, l
	and	#0xF0
	or	#ZC2_NOT_ARP
	ld	l, a
	ld	a, (hl)
	rrca
	rrca
	rrca
	rrca
	and	#0x0F
	add	a, c
	ld	c, a
	jr	_zt_en_final
_zt_en_not_y:
	dec	h
	ld	a, l
	and	#0xF0
	or	#ZC2_NOT_ARP
	ld	l, a
	ld	a, (hl)
	and	#0x0F
	add	a, c
	ld	c, a
	jr	_zt_en_final
_zt_en_not_done:
	dec	h
_zt_en_final:
	ld	a, c
	cp	#96
	jr	c, _zt_en_done
	ld	a, #95
_zt_en_done:
	ex	(sp), hl		; the caller's channel base
	pop	de
	ret

; ---------------------------------------------------------------------------
; fine_pitch: DE = the period -> DE = the adjusted period.  The only
; multiply: a bounded 6-step shift-and-add, only when the pitch is
; non-zero and within +-255.  HL = the channel base (kept normalized).
; ---------------------------------------------------------------------------
_zt_fine_pitch:
	ld	a, l
	or	#ZC_PITCH_LO
	ld	l, a
	ld	c, (hl)
	inc	l
	ld	b, (hl)		; BC = the pitch
	ld	a, l
	and	#0xF0
	ld	l, a
	ld	a, b
	or	a
	jr	nz, _zt_fp_neg
	ld	a, c
	or	a
	ret	z			; zero: unchanged
	xor	a
	ld	(_zt_sign), a
	ld	a, c			; the magnitude
	jr	_zt_fp_mag
_zt_fp_neg:
	inc	a
	ret	nz			; |pitch| > 255: unchanged
	xor	a
	sub	c			; -pitch
	jr	z, _zt_fp_ret		; -256: unchanged
	ld	c, a			; the magnitude survives the sign store
	ld	a, #1
	ld	(_zt_sign), a
	ld	a, c
_zt_fp_mag:
	cp	#64
	jr	c, 1$
	ld	a, #63
1$:
	ld	c, a			; C = the magnitude
	push	hl
	ex	de, hl			; HL = the period
	push	hl
	ld	de, #0
	ld	b, #6
2$:
	srl	c
	jr	nc, 3$
	ex	de, hl
	add	hl, de
	ex	de, hl
3$:
	add	hl, hl
	djnz	2$
	; DE = period * magnitude, HL = period << 6
	ex	de, hl			; HL = the product
	ld	b, #11
4$:
	srl	h
	rr	l
	djnz	4$
	ex	de, hl			; DE = the delta
	pop	hl			; HL = the period
	ld	a, (_zt_sign)
	or	a
	jr	nz, 6$
	or	a
	sbc	hl, de
	ld	a, h
	or	l
	jr	nz, 5$
	ld	hl, #1
	jr	5$
6$:
	add	hl, de
	ld	a, h
	cp	#4
	jr	c, 5$
	ld	hl, #1023
5$:
	ex	de, hl			; DE = the adjusted period
	pop	hl			; the channel base
	ld	a, l
	and	#0xF0
	ld	l, a
	ret
_zt_fp_ret:
	ret

; ---------------------------------------------------------------------------
; ---------------------------------------------------------------------------
; The dirty marker: A = the dirty bits to set.  HL = the channel base.
; ---------------------------------------------------------------------------
_zt_dirty:
	ld	c, a
	ld	a, l
	or	#ZC_DIRTY
	ld	l, a
	ld	a, c
	or	(hl)
	ld	(hl), a
	ld	a, l
	and	#0xF0
	or	#ZC_WORK
	ld	l, a
	set	7, (hl)
	ld	a, l
	and	#0xF0
	ld	l, a
	ret

; ---------------------------------------------------------------------------
; The position advance (the reference's advance_position): the next row,
; or the next order, or DONE at the song's end.
; ---------------------------------------------------------------------------
_zt_advance_position:
	ld	a, (_snd_ztr_row)
	inc	a
	ld	c, a
	ld	hl, (_snd_ztr_pattern_length)
	ld	a, c
	cp	l
	jr	c, _zt_ap_row
	xor	a
	ld	(_snd_ztr_row), a
	ld	a, (_snd_ztr_order)
	inc	a
	ld	c, a
	ld	(_snd_ztr_order), a
	ld	hl, (_snd_ztr_order_count)
	ld	a, c
	cp	l
	ret	c
	ld	a, #1
	ld	(_zt_end_pending), a
	ret
_zt_ap_row:
	ld	a, c
	ld	(_snd_ztr_row), a
	ret

; ---------------------------------------------------------------------------
; void snd_ztr_mute(void): the mask's voices to attenuation 15.
; ---------------------------------------------------------------------------
_snd_ztr_mute::
	ld	hl, (_snd_ztr_mask)
	ld	b, #16
_zt_mute_loop:
	srl	h
	rr	l			; carry = this voice's low-to-high mask bit
	jr	nc, _zt_mute_next
	push	hl
	push	bc
	ld	a, #16
	sub	b
	rlca
	rlca
	rlca
	rlca
	and	#0xF0
	ld	l, a
	ld	a, (_snd_desc_page)
	ld	h, a			; the descriptor base
	ld	a, #15
	call	snd_put_atten
	; the engine's last_attenuation follows the PSG: 15 (the put
	; clobbered L: derive the channel base again)
	ld	a, #16
	sub	b
	rlca
	rlca
	rlca
	rlca
	and	#0xF0
	ld	l, a
	ld	a, (_snd_ztr_core_page)
	ld	h, a
	inc	h
	ld	a, l
	or	#ZC2_LAST_ATT
	ld	l, a
	ld	(hl), #15
	pop	bc
	pop	hl
_zt_mute_next:
	djnz	_zt_mute_loop
	ret

; ---------------------------------------------------------------------------
; void snd_ztr_restore(void): re-render the mask's channels from the
; held state (pause muted them; resume restores them).
; ---------------------------------------------------------------------------
_snd_ztr_restore::
	ld	hl, (_snd_ztr_mask)
	ld	b, #16
_zt_rs_loop:
	srl	h
	rr	l			; carry = this voice's low-to-high mask bit
	jr	nc, _zt_rs_next
	push	hl
	push	bc
	; the voice: 16 - b
	ld	a, #16
	sub	b
	rlca
	rlca
	rlca
	rlca
	and	#0xF0
	ld	l, a
	ld	a, (_snd_ztr_core_page)
	ld	h, a			; the channel base
	; active channels re-render everything; inactive ones only the
	; (silent) attenuation
	ld	a, l
	or	#ZC_FLAGS
	ld	l, a
	bit	0, (hl)
	jr	nz, 1$
	ld	a, #DY_VOLUME
	jr	2$
1$:
	ld	a, #DY_PITCH|DY_NOISE|DY_VOLUME|DY_PHASE
2$:
	call	_zt_dirty
	call	_zt_render
	pop	bc
	pop	hl
_zt_rs_next:
	djnz	_zt_rs_loop
	ret

; ---------------------------------------------------------------------------
; void snd_ztr_reset_channels(void): the reference player_start's
; per-channel initialization, verbatim.
; ---------------------------------------------------------------------------
_snd_ztr_reset_channels::
	xor	a
	ld	(_zt_end_pending), a
	ld	a, (_snd_ztr_core_page)
	ld	h, a
	ld	l, #0
	call	_zt_zero_page
	inc	h
	call	_zt_zero_page
	; the five macro pages
	ld	b, #5
	ld	hl, #_snd_ztr_macro_page
_zt_zc_loop:
	push	bc
	push	hl
	ld	a, (hl)
	ld	h, a
	ld	l, #0
	call	_zt_zero_page
	pop	hl
	pop	bc
	inc	hl
	djnz	_zt_zc_loop
	; player_start, per channel: note and instrument FF, the volumes
	; 15, the dirty all set, the register cache 0/FF/FF.
	ld	a, (_snd_ztr_core_page)
	ld	h, a
	ld	b, #16
	ld	l, #0
_zt_zc_chan:
	ld	a, l
	or	#ZC_NOTE
	ld	l, a
	ld	(hl), #0xFF
	ld	a, l
	and	#0xF0
	or	#ZC_INSTRUMENT
	ld	l, a
	ld	(hl), #0xFF
	ld	a, l
	and	#0xF0
	or	#ZC_BASE_VOL
	ld	l, a
	ld	(hl), #15
	ld	a, l
	and	#0xF0
	or	#ZC_MACRO_VOL
	ld	l, a
	ld	(hl), #15
	ld	a, l
	and	#0xF0
	or	#ZC_DIRTY
	ld	l, a
	ld	(hl), #DY_PITCH|DY_VOLUME|DY_NOISE|DY_PHASE
	ld	a, l
	and	#0xF0
	ld	l, a
	inc	h
	ld	a, l
	or	#ZC2_LAST_PER_LO
	ld	l, a
	ld	(hl), #0
	inc	l
	ld	(hl), #0
	ld	a, l
	and	#0xF0
	or	#ZC2_LAST_ATT
	ld	l, a
	ld	(hl), #0xFF
	inc	l
	ld	(hl), #0xFF
	dec	h
	ld	a, l
	and	#0xF0
	add	a, #16
	ld	l, a
	djnz	_zt_zc_chan
	ret

; One 256-byte page at HL, zeroed.
_zt_zero_page:
	ld	(hl), #0
	ld	e, l
	ld	d, h
	inc	de
	ld	bc, #0x00FF
	ldir
	ret
