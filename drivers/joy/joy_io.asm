; -----------------------------------------------------------------------------
; KonaOS JOY resident driver (drivers/joy, MOD module).
;
; The board's two controller latches behind the JOY protocol of
; abi/kos/protocol.h (class KOS_CLASS_JOY, instance 0, version
; KOS_PROTOCOL_VERSION), published as `/dev/input/joy/0` (object 0) and
; `/dev/input/joy/1` (object 1).  The IO controller loads each latch with
; its pad's ColecoVision byte; reads in E0h-FFh are the latches.  The IO
; board's U6 74HC138 wires only A0 (A1 and A2 are tied low), so an even
; port is pad 0 and an odd port pad 1: FCh is pad 0 and FFh pad 1 (the
; block's writes are the sound card's, drivers/sound).
;
;   KJOY_OP_READ  arg0 = the pad; one IN from its port into payload[0];
;   KJOY_OP_INFO  the pad count and each pad's port.
;
; The driver owns those two read ports and nothing else.  It keeps no
; state and does no decoding: the byte is delivered as the latch holds
; it, active low.  There is no init, no release notification and no
; event source.  The dispatch is the whole driver, in assembly, so it
; stays a few dozen hot bytes.
; -----------------------------------------------------------------------------

	.module joy_io
	.include "hardware.inc"

	.globl _joy_driver_dispatch

; abi/kos/protocol.h's numbers.  The frame (struct kos_ipc_frame,
; abi/kos/ipc.h) is op, flags, desc, arg0, arg1 (two bytes), then the
; 24-byte payload at offset 6.
KJOY_PAD_MAX      = 2
KOS_JOY_EINVAL    = 0xFFFE		; -KOS_EINVAL

	.area _CODE

; ---------------------------------------------------------------------------
; int16_t joy_driver_dispatch(struct kos_ipc_frame *frame);
;   sdcccall(1): HL = frame, the result in DE.  Non-zero flags, an
;   unknown operation and a pad above 1 are -KOS_EINVAL.
; ---------------------------------------------------------------------------
_joy_driver_dispatch::
	ld	de,#KOS_JOY_EINVAL
	ld	a,(hl)			; op
	inc	hl
	ld	b,(hl)			; flags
	inc	hl
	inc	hl
	ld	c,(hl)			; arg0: the pad
	inc	b
	dec	b
	ret	nz
	inc	hl
	inc	hl
	inc	hl			; HL = payload
	or	a			; KJOY_OP_READ (0)
	jr	nz,joy_driver_info
	ld	a,c
	cp	#KJOY_PAD_MAX
	ret	nc
	ld	c,#KOS_JOY0_PORT
	or	a
	jr	z,joy_driver_in
	ld	c,#KOS_JOY1_PORT
joy_driver_in:
	in	a,(c)			; the latch byte, as the controller set it
	ld	(hl),a
	ld	de,#0
	ret
joy_driver_info:
	dec	a			; KJOY_OP_INFO (1)
	ret	nz
	ld	(hl),#KJOY_PAD_MAX	; payload[KJOY_INFO_COUNT]
	inc	hl
	ld	(hl),#KOS_JOY0_PORT	; payload[KJOY_INFO_PORTS + 0]
	inc	hl
	ld	(hl),#KOS_JOY1_PORT	; payload[KJOY_INFO_PORTS + 1]
	ld	e,a			; A = 0
	ld	d,a
	ret
