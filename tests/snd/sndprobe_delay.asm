; -----------------------------------------------------------------------------
; The sound-series harness self-test's calibrated delay (tests/snd).
;
;   void sndprobe_delay(uint8_t iterations);   A = iterations
;
; Interrupts are off around the loop, so the interval between two DAC
; markers is exactly 30 + 13 * n T-states:
;   di 4, ld b,a 4, djnz taken 13 (last 8), ei 4, ret 10.
; -----------------------------------------------------------------------------

	.module sndprobe_delay

	.globl _sndprobe_delay

	.area _CODE

_sndprobe_delay::
	di
	ld	b,a
loop$:
	djnz	loop$
	ei
	ret
