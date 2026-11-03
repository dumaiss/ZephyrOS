; -----------------------------------------------------------------------------
; KonaOS libkos: a game controller's latch byte, read directly.
;
; uint8_t joy_read(int joy);  HL = the handle, A = the byte
;   joy_open stored the pad's port at joy_port[handle] (joy.c); this is
;   one IN from it, with no call into the driver, because a game reads
;   its pads every frame.  The handle's low four bits index the table,
;   and a slot joy_open did not fill (port 0) reads JOY_IDLE, so a stray
;   handle never reads another device's port.  Its own archive member: a
;   program that reads only through joy_read_call never links it.
; -----------------------------------------------------------------------------

	.module joy_read
	.globl _joy_read
	.globl _joy_port

JOY_IDLE = 0x7F

	.area _CODE

_joy_read::
	ld	a,l
	and	#0x0F
	ld	e,a
	ld	d,#0
	ld	hl,#_joy_port
	add	hl,de			; HL = &joy_port[handle & 15]
	ld	c,(hl)
	ld	a,#JOY_IDLE
	inc	c
	dec	c
	ret	z			; not a joy_open handle
	in	a,(c)
	ret
