; Acceptance-only caller for libsnd's internal descriptor writer ABI
; (HL = the voice's descriptor base, in the 256-byte-aligned page).

	.module sndtest_put
	.globl _sndtest_put_primitives
	.globl _snd_desc_page
	.globl snd_put_period
	.globl snd_put_atten
	.globl snd_put_noise

	.area _CODE

_sndtest_put_primitives::
	; Equal tone period and attenuation values write nothing.
	ld	a, (_snd_desc_page)
	ld	h, a
	ld	l, #0x00		; voice 0
	ld	de, #0
	call	snd_put_period
	ld	l, #0x00
	ld	a, #15
	call	snd_put_atten
	; Equal unforced noise writes nothing; forced noise writes E0 once.
	ld	l, #0x30		; voice 3, the chip's noise voice
	ld	a, #0xE0
	ld	b, #0
	call	snd_put_noise
	ld	l, #0x30
	ld	a, #0xE0
	ld	b, #1
	call	snd_put_noise
	ret
