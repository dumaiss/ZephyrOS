; -----------------------------------------------------------------------------
; SIO0/B low-level port primitives for the SIO driver.
;
; These are deliberately trivial: raw Zephyr-80 port access only, no
; policy.  They live in the bank-7 loadable region with the rest of the
; driver (the early console keeps its own independent common-resident
; access path).
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
