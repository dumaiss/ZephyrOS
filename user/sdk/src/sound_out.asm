; -----------------------------------------------------------------------------
; KonaOS libkos: one direct port write for an exclusive sound holder.
;
; void sound_out(unsigned char port, unsigned char value);  A = port, L = value
;   C cannot name a port chosen at run time; the holder writes the ports
;   sound_info reported.  Its own archive member: a program that writes
;   only through sound_write never links it.
; -----------------------------------------------------------------------------

	.module sound_out
	.globl _sound_out

	.area _CODE

_sound_out::
	ld	c,a
	out	(c),l
	ret
