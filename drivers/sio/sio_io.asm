; -----------------------------------------------------------------------------
; SIO0/B low-level port primitives for the SIO driver.
;
; These are deliberately trivial: raw Zephyr-80 port access only, no
; policy.  They live in the driver's hot segment with the rest of the
; driver (the early console keeps its own independent common-resident
; access path).  sio0b_setup is the channel's programming after its
; reset; the boot runs it once, and the end of an exclusive hold runs it
; again through sio0b_restore, because the holder may have left any
; register in any state.
;
; SDCC sdcccall(1) convention: first uint8_t argument in A, uint8_t
; result in L.
; -----------------------------------------------------------------------------

	.module sio_io
	.include "hardware.inc"

	.globl _sio0b_status
	.globl _sio0b_write_ctrl
	.globl _sio0b_read_data
	.globl _sio0b_write_data
	.globl _sio0b_setup
	.globl _sio0b_restore

	.area _CODE

; uint8_t sio0b_status(void);  read RR0 from the control port.
_sio0b_status::
	in	a,(KOS_SIO0B_CTRL)
	ld	l,a
	ret

; void sio0b_write_ctrl(uint8_t value);  A = value.
_sio0b_write_ctrl::
	out	(KOS_SIO0B_CTRL),a
	ret

; uint8_t sio0b_read_data(void);  read one RX byte.
_sio0b_read_data::
	in	a,(KOS_SIO0B_DATA)
	ld	l,a
	ret

; void sio0b_write_data(uint8_t value);  A = value.
_sio0b_write_data::
	out	(KOS_SIO0B_DATA),a
	ret

; void sio0b_setup(void);  WR4, WR3, WR5 (RTS asserted), WR1, in the
; documented order: the mode first, then the enables, then interrupts
; off.  Each pair is a WR0 register select and the value.
_sio0b_setup::
	ld	hl,#sio0b_setup_bytes
	ld	bc,#((sio0b_setup_end - sio0b_setup_bytes) << 8) | KOS_SIO0B_CTRL
	otir
	ret
sio0b_setup_bytes:
	.db	KOS_SIO_WR_SELECT_WR4, KOS_SIO_WR4_X16_1STOP_NOPARITY
	.db	KOS_SIO_WR_SELECT_WR3, KOS_SIO_WR3_RX8_NO_AUTO_ENABLES
	.db	KOS_SIO_WR_SELECT_WR5, KOS_SIO_WR5_TX8_DTR_RTS_ASSERT
	.db	KOS_SIO_WR_SELECT_WR1, KOS_SIO_WR1_INT_DISABLED
sio0b_setup_end:

; void sio0b_restore(void);  the setup, then the receiver drained: every
; byte the FIFO holds is read and dropped, at most 256 reads, so a line
; that keeps sending cannot hold the call.
_sio0b_restore::
	call	_sio0b_setup
	ld	b,#0
sio0b_restore_drain:
	in	a,(KOS_SIO0B_CTRL)
	rrca				; RR0 D0: a received byte is pending
	ret	nc
	in	a,(KOS_SIO0B_DATA)
	djnz	sio0b_restore_drain
	ret
