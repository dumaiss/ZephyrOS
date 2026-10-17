; unsigned int strlen(const char *s);  HL = s, result in DE.
; CPIR from BC = 0 counts down to the terminator: the length is -BC - 1.
	.module kos_strlen
	.globl _strlen
	.area _CODE
_strlen::
	xor	a
	ld	b,a
	ld	c,a
	cpir
	ld	a,c
	cpl
	ld	e,a
	ld	a,b
	cpl
	ld	d,a
	ret
