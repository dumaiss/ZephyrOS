; -----------------------------------------------------------------------------
; libsnd: the PCM DPCM4 block decoder (docs/snd-sdk.md section 12.2).
;
; void snd_pcm_dpcm4(const uint8_t *src, uint8_t *page);
;   HL = the block's nibble body, DE = a ring page (E = 0).  Decodes the
;   block's 256 samples into the page, starting from the predictor in
;   _snd_pcm_pred and leaving the last sample there.
;
; The body is a nibble stream, high nibble first.  A nibble n other than 8
; adds the delta (n XOR 8) - 8 (-7..+7) to the predictor; 8 is an escape:
; the next two nibbles, high then low, are the sample itself.  The decoder
; stops after 256 samples, so a body's padding nibble is never read.
;
; Cost: about 80 T-states a sample (two samples per byte on the fast path).
; Registers: C the predictor, B the current byte; clobbers AF, BC, DE, HL.
; -----------------------------------------------------------------------------

	.module snd_pcm_dec

	.globl _snd_pcm_dpcm4
	.globl _snd_pcm_pred

	.area _CODE

_snd_pcm_dpcm4::
	ld	a,(_snd_pcm_pred)
	ld	c,a
dp_byte:
	ld	b,(hl)
	inc	hl
	ld	a,b
	rrca
	rrca
	rrca
	rrca
	and	#0x0F
	cp	#8
	jr	z,dp_esc_hi
	xor	#8
	sub	#8
	add	a,c
	ld	c,a
	ld	(de),a
	inc	e
	jr	z,dp_done
dp_low:
	ld	a,b
	and	#0x0F
	cp	#8
	jr	z,dp_esc_lo
	xor	#8
	sub	#8
	add	a,c
	ld	c,a
	ld	(de),a
	inc	e
	jr	nz,dp_byte
dp_done:
	ld	a,c
	ld	(_snd_pcm_pred),a
	ret

; An escape in the high nibble: the sample's high half is this byte's low
; nibble and its low half the next byte's high nibble; that byte's low
; nibble is the next code.
dp_esc_hi:
	ld	a,b
	rlca
	rlca
	rlca
	rlca
	and	#0xF0
	ld	c,a
	ld	b,(hl)
	inc	hl
	ld	a,b
	rrca
	rrca
	rrca
	rrca
	and	#0x0F
	or	c
	ld	c,a
	ld	(de),a
	inc	e
	jr	z,dp_done
	jr	dp_low

; An escape in the low nibble: the sample is the next byte, whole, and the
; stream is byte-aligned again.
dp_esc_lo:
	ld	a,(hl)
	inc	hl
	ld	c,a
	ld	(de),a
	inc	e
	jr	nz,dp_byte
	jr	dp_done
