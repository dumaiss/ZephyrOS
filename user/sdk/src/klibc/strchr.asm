; char *strchr(const char *s, int c);  HL = s, DE = c, result in DE: the
; first byte equal to (char)c (the terminator when c is 0), else 0.
	.module kos_strchr
	.globl _strchr
	.area _CODE
_strchr::
	ld	a,(hl)
	cp	e
	jr	z,strchr_found
	or	a
	jr	z,strchr_none
	inc	hl
	jr	_strchr
strchr_found:
	ex	de,hl
	ret
strchr_none:
	ld	de,#0
	ret
