; -----------------------------------------------------------------------------
; Afternoon Blend sound-card port I/O (drivers/sound, MOD module).
;
; Every E0h-E4h access lives here; sound_proto.c owns the protocol
; dispatch, the lifecycle and the response records.  The card has four
; SN76489 PSGs at E0h-E3h and a DAC at E4h.
;
; The reset is the one recovery path: `9Fh BFh DFh FFh` to each PSG
; (attenuation 15 on all three tone channels and the noise channel, which
; silences the chip) then the DAC's neutral code, 80h.  It runs at the
; driver's init, at an exclusive acquire, at KSOUND_OP_RESET and at the
; release of an exclusive hold.  The driver keeps no shadow of the
; chips' registers: a direct writer may have changed any of them, and
; the reset recovers from that state.
;
; The write sequence validates the whole caller-supplied pair list
; before the first OUT (a target above KSOUND_TARGET_DAC is refused), then
; performs it, both passes through the 64-byte DRIVER_IO batch a chunk at
; a time, so one call is one sequence.
; -----------------------------------------------------------------------------

	.module sound_io
	.include "hardware.inc"

	.globl _sound_io_reset
	.globl _sound_io_write_sequence
	.globl _sound_driver_api

; The SN76489's silence bytes: latch and data for tone generator 0 at
; attenuation 15, the same for tone generators 1 and 2, and the noise
; generator's attenuation 15.
KOS_SOUND_SILENCE_0 = 0x9F
KOS_SOUND_SILENCE_1 = 0xBF
KOS_SOUND_SILENCE_2 = 0xDF
KOS_SOUND_SILENCE_3 = 0xFF
KOS_SOUND_DAC_NEUTRAL = 0x80
KOS_SOUND_TARGET_MAX = KOS_SOUND_DAC_PORT - KOS_SOUND_PSG0_PORT
KOS_SOUND_EINVAL     = 0xFFFE

	.area _CODE

; The write sequence reaches the bootstrap interface's copy_in through
; its function pointer; this is the local indirect call, not the shared
; SDCC runtime object.
sound_io_call_hl:
	jp	(hl)

; ---------------------------------------------------------------------------
; void sound_io_reset(void);
;   Silence each PSG, then park the DAC at midscale.  The PSG ports are
;   written in order through one port index, so the sequence is the same
;   from any register state.
; ---------------------------------------------------------------------------
_sound_io_reset::
	ld	c,#KOS_SOUND_PSG0_PORT
sound_io_reset_psg:
	ld	hl,#sound_io_silence
	ld	b,#4
	otir
	inc	c
	ld	a,c
	cp	#KOS_SOUND_DAC_PORT
	jr	nz,sound_io_reset_psg
	ld	a,#KOS_SOUND_DAC_NEUTRAL
	out	(KOS_SOUND_DAC_PORT),a
	ret
sound_io_silence:
	.db	KOS_SOUND_SILENCE_0, KOS_SOUND_SILENCE_1
	.db	KOS_SOUND_SILENCE_2, KOS_SOUND_SILENCE_3

; ---------------------------------------------------------------------------
; int16_t sound_io_write_sequence(uint8_t desc, uint16_t length);
;   sdcccall(1): A = desc, DE = length.  Two passes over the descriptor's
;   {target, value} pairs, 64 bytes a chunk, each chunk copied into
;   sound_batch.  The validate pass checks every target, last chunk
;   first, so it ends holding the first chunk; the write pass then
;   performs the whole sequence in order from that chunk, copying only the
;   chunks after it: nothing is written unless all of it is valid, and a
;   sequence of up to 32 pairs moves once.  Returns length, or
;   -KOS_EINVAL with no write made.
;
;   The frame holds desc, length and done; -8/-7 hold the chunk size.  The
;   bootstrap copy_in is (*sound_driver_api).copy_in, the first table entry.
; ---------------------------------------------------------------------------
_sound_io_write_sequence::
	push	ix
	ld	ix,#0
	add	ix,sp
	ld	hl,#-8
	add	hl,sp
	ld	sp,hl
	ld	-2(ix),a		; desc
	ld	-4(ix),e
	ld	-3(ix),d		; length
	dec	de
	ld	a,e
	and	#0xC0
	ld	-6(ix),a
	ld	-5(ix),d		; done = the last chunk's offset
sound_io_validate_loop:
	call	sound_io_next_chunk
	jr	nz,sound_io_invalid
	ld	hl,#_sound_batch
	ld	b,-8(ix)
	srl	b			; pair count = chunk / 2
sound_io_validate_pair:
	ld	a,(hl)
	cp	#(KOS_SOUND_TARGET_MAX + 1)
	jr	nc,sound_io_invalid
	inc	hl
	inc	hl
	djnz	sound_io_validate_pair
	ld	a,-6(ix)
	sub	#0x40
	ld	-6(ix),a
	ld	a,-5(ix)
	sbc	a,#0
	ld	-5(ix),a		; done -= 64
	jr	nc,sound_io_validate_loop
	xor	a
	ld	-6(ix),a
	ld	-5(ix),a		; done = 0: the first chunk, in the batch
; The write pass.  Target 0-3 selects PSG0-PSG3 and 4 the DAC, whose
; ports are consecutive on this card; OUTI sends the value and counts the
; pair.
sound_io_write_begin:
	ld	hl,#_sound_batch
	ld	b,-8(ix)
	srl	b			; pair count = chunk / 2
sound_io_write_pair:
	ld	a,(hl)			; target
	add	a,#KOS_SOUND_PSG0_PORT
	ld	c,a
	inc	hl
	outi				; value
	jr	nz,sound_io_write_pair
	ld	a,-6(ix)
	add	a,-8(ix)
	ld	-6(ix),a
	ld	a,-5(ix)
	adc	a,-7(ix)
	ld	-5(ix),a		; done += chunk
	ld	a,-6(ix)
	sub	-4(ix)
	ld	a,-5(ix)
	sbc	a,-3(ix)
	jr	nc,sound_io_written
	call	sound_io_next_chunk
	jr	z,sound_io_write_begin
sound_io_invalid:
	ld	de,#KOS_SOUND_EINVAL
	jr	sound_io_sequence_exit
sound_io_written:
	ld	e,-4(ix)
	ld	d,-3(ix)
sound_io_sequence_exit:
	ld	sp,ix
	pop	ix
	ret

; chunk = min(length - done, 64), copied into sound_batch.  Z is set on
; success; the chunk size is left at -8/-7.
sound_io_next_chunk:
	ld	a,-4(ix)
	sub	-6(ix)
	ld	-8(ix),a
	ld	a,-3(ix)
	sbc	a,-5(ix)
	ld	-7(ix),a
	ld	a,#0x40
	cp	-8(ix)
	ld	a,#0
	sbc	a,-7(ix)
	jr	nc,sound_io_chunk_sized
	ld	-8(ix),#0x40
	ld	-7(ix),#0
sound_io_chunk_sized:
	ld	l,-8(ix)
	ld	h,-7(ix)
	push	hl
	ld	hl,#_sound_batch
	push	hl
	ld	e,-6(ix)
	ld	d,-5(ix)
	ld	hl,(_sound_driver_api)
	inc	hl			; skip call, the first entry
	inc	hl
	ld	a,(hl)			; copy_in's address
	inc	hl
	ld	h,(hl)
	ld	l,a
	ld	a,-2(ix)		; desc (loading the pointer uses A)
	call	sound_io_call_hl
	bit	7,d
	ret

	.area _DRIVER_IO
_sound_batch::
	.ds	64
