; -----------------------------------------------------------------------------
; IOC command-lane port I/O and streaming loops (drivers/ioc, ZMOD module).
;
; All SIO1/B access for the IOC class lives here.  The C side (ioc.c) owns
; framing, CRC, validation, and state; this file owns every `in`/`out`
; and the timing-critical byte loops.
;
; Shared block passed by pointer in HL (sdcccall(1), single 16-bit
; argument):
;
;   struct ioc_io_block {
;       uint8_t *buf;    0..1   TX packet / raw RX frame destination
;       uint8_t  len;    2      TX: total packet byte count
;       uint8_t  hunt;   3      TX: 1 = arm RX with Enter Hunt Phase
;       uint8_t  resume; 4      RX: nonzero = re-enable interrupts for
;                               the reply wait (transaction entered with
;                               interrupts enabled)
;   };
;
; Return values (sdcccall(1): 8-bit result in A):
;   ioc_io_wait_ready     0 = COMMAND_READY seen, 1 = bounded timeout
;   ioc_io_tx             0 = packet + trailers sent, RTS released, RX
;                            armed; 1 = byte timeout (RTS released)
;   ioc_io_rx             0 = raw frame captured; 1 = no first byte;
;                         2 = marker-scan byte timeout; 3 = scan budget
;                            exhausted; 4 = body byte timeout;
;                         5 = declared length out of range
;   ioc_io_status_sample  ZOS_IOC_HID_DOORBELL_MASK or 0
;   ioc_io_irq_save_disable  previous IFF2 state (1 = enabled)
;
; Every wait is bounded by the verified platform budgets.  Interrupts:
; the C caller masks around the transaction and restores the exact entry
; state; ioc_io_rx may re-enable them for the long MCU wait only when the
; entry state was enabled, and masks again before consuming a byte.
; -----------------------------------------------------------------------------

	.module ioc_io
	.include "zephyr80/hardware.inc"

	.globl _ioc_io_ctrl_write
	.globl _ioc_io_ctrl_read
	.globl _ioc_io_data_write
	.globl _ioc_io_data_read
	.globl _ioc_io_wait_ready
	.globl _ioc_io_tx
	.globl _ioc_io_rx
	.globl _ioc_io_status_sample
	.globl _ioc_io_irq_save_disable
	.globl _ioc_io_irq_restore

	.area _CODE

; ---------------------------------------------------------------------------
; Raw port primitives.
; ---------------------------------------------------------------------------

; void ioc_io_ctrl_write(uint8_t value);  A = value.
_ioc_io_ctrl_write::
	out	(ZOS_SIO1B_CTRL),a
	ret

; uint8_t ioc_io_ctrl_read(void);  A = L = RR0.
_ioc_io_ctrl_read::
	in	a,(ZOS_SIO1B_CTRL)
	ld	l,a
	ret

; void ioc_io_data_write(uint8_t value);  A = value.
_ioc_io_data_write::
	out	(ZOS_SIO1B_DATA),a
	ret

; uint8_t ioc_io_data_read(void);  A = L = one data byte.
_ioc_io_data_read::
	in	a,(ZOS_SIO1B_DATA)
	ld	l,a
	ret

; ---------------------------------------------------------------------------
; uint8_t ioc_io_wait_ready(void);
;   Wait for COMMAND_READY on /DCDB (RR0 D3).  Each poll issues WR0 Reset
;   External/Status first: RR0 latches its modem bits on change, so a
;   stale latch could report a level the pin no longer carries.
;   Out: A = 0 ready, 1 bounded timeout.  Clobbers AF, BC, DE.
; ---------------------------------------------------------------------------
_ioc_io_wait_ready::
	ld	b,#ZOS_IOC_CMD_READY_OUTER
ioc_ready_outer:
	ld	de,#ZOS_IOC_CMD_READY_TIMEOUT
ioc_ready_wait:
	ld	a,#ZOS_IOC_WR0_RESET_EXT_STATUS
	out	(ZOS_SIO1B_CTRL),a
	in	a,(ZOS_SIO1B_CTRL)
	and	#ZOS_IOC_CMD_READY_MASK
	jr	nz,ioc_ready_ok
	dec	de
	ld	a,d
	or	e
	jr	nz,ioc_ready_wait
	djnz	ioc_ready_outer
	ld	a,#1
	ret
ioc_ready_ok:
	xor	a
	ret

; ---------------------------------------------------------------------------
; uint8_t ioc_io_status_sample(void);
;   Unlatch RR0, then sample the /CTSB HID doorbell.  Returns the mask or
;   0; no other RR0 bit ever leaves this module.
; ---------------------------------------------------------------------------
_ioc_io_status_sample::
	ld	a,#ZOS_IOC_WR0_RESET_EXT_STATUS
	out	(ZOS_SIO1B_CTRL),a
	in	a,(ZOS_SIO1B_CTRL)
	and	#ZOS_IOC_HID_DOORBELL_MASK
	ld	l,a
	ret

; ---------------------------------------------------------------------------
; uint8_t ioc_io_tx(struct ioc_io_block *io);
;   One command request, in the deployed order:
;     enable TX (RTS off) -> preload A5 -> EOM reset -> assert RTS ->
;     stream remaining packet bytes -> two FFh trailers -> release RTS by
;     disabling TX -> arm RX (error latch cleared, channel untouched,
;     Enter Hunt only while io->hunt is set).
;   The C caller holds interrupts masked across this call, so the whole
;   stream is a bounded critical section.
;   Out: A = 0 success, 1 byte timeout.  Clobbers AF, BC, DE.
; ---------------------------------------------------------------------------
_ioc_io_tx::
	ld	e,(hl)			; buf low
	inc	hl
	ld	d,(hl)			; buf high  -> DE = packet
	inc	hl			; block + 2
	ld	b,(hl)			; B = packet length
	inc	hl			; HL -> io->hunt (kept for the arm)

	; Restore the documented idle programming: every previous
	; transaction ended with WR5 = 00h (TX disabled).
	ld	a,#0x05
	out	(ZOS_SIO1B_CTRL),a
	ld	a,#ZOS_IOC_WR5_TX_RTS_OFF
	out	(ZOS_SIO1B_CTRL),a

	; PRELOAD, THEN GRANT.  Nothing shifts until the MCU clocks, so the
	; preloaded A5 waits in the transmitter; asserting RTS against an
	; empty transmitter would emit FFh underrun fill first.
	ld	a,(de)
	ld	c,a
	call	ioc_put_byte
	jp	c,ioc_tx_fail

	; First byte is buffered: reset the Tx Underrun/EOM latch so the
	; transmitter leaves idle marking and shifts the preloaded byte.
	ld	a,#ZOS_IOC_WR0_RESET_EOM
	out	(ZOS_SIO1B_CTRL),a

	; Grant.  RTS is a held "unacknowledged request" level.
	ld	a,#0x05
	out	(ZOS_SIO1B_CTRL),a
	ld	a,#ZOS_IOC_WR5_TX_RTS_ON
	out	(ZOS_SIO1B_CTRL),a

	; The remaining packet bytes.
	inc	de
	dec	b
ioc_tx_body:
	ld	a,(de)
	ld	c,a
	call	ioc_put_byte
	jp	c,ioc_tx_fail
	inc	de
	djnz	ioc_tx_body

	; Two FFh trailers so the final frame byte has left the transmitter
	; before the release disables it.
	ld	b,#ZOS_IOC_SEND_TRAILER_BYTES
ioc_tx_trailer:
	ld	c,#ZOS_IOC_SEND_TRAILER_FILL
	call	ioc_put_byte
	jp	c,ioc_tx_fail
	djnz	ioc_tx_trailer

	; Release RTS by disabling TX: an External-Sync transmitter never
	; reports empty, so clearing only the RTS bit would latch /RTS low.
	ld	a,#0x05
	out	(ZOS_SIO1B_CTRL),a
	ld	a,#ZOS_IOC_WR5_IDLE
	out	(ZOS_SIO1B_CTRL),a

	; Arm the receiver before interrupts return.  Clear the error latch
	; without resetting the channel, then enable RX - Enter Hunt only
	; for the first establishment (HL still addresses io->hunt).
	ld	a,#ZOS_IOC_WR0_RESET_ERROR
	out	(ZOS_SIO1B_CTRL),a
	ld	a,(hl)
	or	a
	ld	b,#ZOS_IOC_WR3_RX_NO_HUNT
	jr	z,ioc_tx_arm
	ld	b,#ZOS_IOC_WR3_RX_HUNT
ioc_tx_arm:
	ld	a,#0x03
	out	(ZOS_SIO1B_CTRL),a
	ld	a,b
	out	(ZOS_SIO1B_CTRL),a
	xor	a
	ret

ioc_tx_fail:
	; Release RTS exactly as the success path does, then report the
	; bounded timeout.  The receiver is deliberately left alone: only a
	; channel reset, a receiver disable, or Enter Hunt ends character
	; assembly, so a partial request cannot destroy the boundary.
	ld	a,#0x05
	out	(ZOS_SIO1B_CTRL),a
	ld	a,#ZOS_IOC_WR5_IDLE
	out	(ZOS_SIO1B_CTRL),a
	ld	a,#1
	ret

; ---------------------------------------------------------------------------
; uint8_t ioc_io_rx(struct ioc_io_block *io);
;   Capture one reply for the current exchange into io->buf as the raw
;   wire frame LEN_LO LEN_HI TYPE SEQ STATUS DATA... CRC_HI CRC_LO.
;   Entered with interrupts masked; uses io->resume to restore them (when
;   the transaction entered enabled) for the long MCU wait, masks again
;   at the first receive-ready indication, and always returns masked.
;   The receiver stays enabled throughout; no channel reset, no disable.
;   Out: A = 0 captured, 1/2/4 byte-wait timeouts, 3 scan exhausted,
;   5 declared length out of range.  Clobbers AF, BC, DE.
; ---------------------------------------------------------------------------
_ioc_io_rx::
	ld	e,(hl)			; buf low
	inc	hl
	ld	d,(hl)			; buf high
	inc	hl			; block + 2
	inc	hl			; block + 3
	inc	hl			; block + 4 = resume
	ld	a,(hl)
	ex	de,hl			; HL = destination; DE free
	push	af			; stash the resume flag

	; Clear the error latch (not the channel, not the receiver).
	ld	a,#ZOS_IOC_WR0_RESET_ERROR
	out	(ZOS_SIO1B_CTRL),a

	; Drain at most the FIFO depth plus one stale full-duplex byte.
	ld	b,#ZOS_IOC_RX_DRAIN_LIMIT
ioc_rx_drain:
	xor	a
	out	(ZOS_SIO1B_CTRL),a
	in	a,(ZOS_SIO1B_CTRL)
	and	#ZOS_SIO_RR0_RX_AVAILABLE
	jr	z,ioc_rx_drained
	in	a,(ZOS_SIO1B_DATA)
	djnz	ioc_rx_drain
ioc_rx_drained:
	ld	a,#ZOS_IOC_WR0_RESET_ERROR
	out	(ZOS_SIO1B_CTRL),a

	; Restore interrupts for the potentially long MCU reply wait when
	; the transaction entered with them enabled.
	pop	af
	or	a
	jr	z,ioc_rx_wait_first
	ei
ioc_rx_wait_first:
	ld	b,#ZOS_IOC_CMD_BYTE_OUTER
ioc_rx_wait_outer:
	ld	de,#ZOS_IOC_CMD_BYTE_TIMEOUT
ioc_rx_wait:
	in	a,(ZOS_SIO1B_CTRL)
	and	#ZOS_SIO_RR0_RX_AVAILABLE
	jr	nz,ioc_rx_streaming
	dec	de
	ld	a,d
	or	e
	jr	nz,ioc_rx_wait
	djnz	ioc_rx_wait_outer
	di
	ld	a,#1
	ret
ioc_rx_streaming:
	di

	; Bounded search for the complete A5 5A marker.  The previous
	; reply's trailing FFh can become FIFO-visible only under these
	; clocks, so the marker - not byte zero - defines the frame start.
	ld	b,#ZOS_IOC_CMD_REPLY_SCAN_LIMIT
ioc_rx_scan0:
	call	ioc_get_byte
	jr	c,ioc_rx_marker_timeout
	cp	#ZOS_IOC_PACKET_SYNC0
	jr	z,ioc_rx_scan1
	djnz	ioc_rx_scan0
	jr	ioc_rx_scan_fail
ioc_rx_scan1:
	call	ioc_get_byte
	jr	c,ioc_rx_marker_timeout
	cp	#ZOS_IOC_PACKET_SYNC1
	jr	z,ioc_rx_header
	cp	#ZOS_IOC_PACKET_SYNC0
	jr	z,ioc_rx_scan1		; overlapping A5 A5 5A
	djnz	ioc_rx_scan0
ioc_rx_scan_fail:
	ld	a,#3
	ret
ioc_rx_marker_timeout:
	ld	a,#2
	ret

	; Five-byte header: LEN_LO LEN_HI TYPE SEQ STATUS.
ioc_rx_header:
	ld	b,#5
ioc_rx_header_loop:
	call	ioc_get_byte
	jr	c,ioc_rx_body_timeout
	ld	(hl),a
	inc	hl
	djnz	ioc_rx_header_loop

	; LEN must be 3..29 with a zero high byte; LEN counts TYPE + SEQ +
	; STATUS + DATA.  HL now points one past the header.
	ld	d,h
	ld	e,l
	dec	de
	dec	de
	dec	de
	dec	de			; DE -> LEN_HI
	ld	a,(de)
	or	a
	jr	nz,ioc_rx_bad_header
	dec	de			; DE -> LEN_LO
	ld	a,(de)
	cp	#ZOS_IOC_PACKET_FIXED_LEN
	jr	c,ioc_rx_bad_header
	cp	#(ZOS_IOC_PACKET_FIXED_LEN + ZOS_IOC_CMD_MAX_DATA + 1)
	jr	nc,ioc_rx_bad_header
	sub	#ZOS_IOC_PACKET_FIXED_LEN
	or	a
	jr	z,ioc_rx_crc
	ld	b,a
ioc_rx_data_loop:
	call	ioc_get_byte
	jr	c,ioc_rx_body_timeout
	ld	(hl),a
	inc	hl
	djnz	ioc_rx_data_loop
ioc_rx_crc:
	call	ioc_get_byte
	jr	c,ioc_rx_body_timeout
	ld	(hl),a
	inc	hl
	call	ioc_get_byte
	jr	c,ioc_rx_body_timeout
	ld	(hl),a
	xor	a
	ret
ioc_rx_bad_header:
	ld	a,#5
	ret
ioc_rx_body_timeout:
	ld	a,#4
	ret

; ---------------------------------------------------------------------------
; Internal byte helpers.  Both preserve HL and B (the caller's scan/data
; counter) across the call.
; ---------------------------------------------------------------------------

; In: C = byte.  Out: carry set on timeout.  Clobbers AF, DE.
ioc_put_byte:
	push	de
	push	bc
	ld	b,#ZOS_IOC_CMD_BYTE_OUTER
ioc_pb_outer:
	ld	de,#ZOS_IOC_CMD_BYTE_TIMEOUT
ioc_pb_wait:
	in	a,(ZOS_SIO1B_CTRL)
	and	#ZOS_SIO_RR0_TX_EMPTY
	jr	nz,ioc_pb_ready
	dec	de
	ld	a,d
	or	e
	jr	nz,ioc_pb_wait
	djnz	ioc_pb_outer
	pop	bc
	pop	de
	scf
	ret
ioc_pb_ready:
	pop	bc
	ld	a,c
	out	(ZOS_SIO1B_DATA),a
	pop	de
	or	a
	ret

; Out: carry set on timeout; else A = received byte.  Clobbers AF, DE.
ioc_get_byte:
	push	bc
	ld	b,#ZOS_IOC_CMD_BYTE_OUTER
ioc_gb_outer:
	ld	de,#ZOS_IOC_CMD_BYTE_TIMEOUT
ioc_gb_wait:
	in	a,(ZOS_SIO1B_CTRL)
	and	#ZOS_SIO_RR0_RX_AVAILABLE
	jr	nz,ioc_gb_ready
	dec	de
	ld	a,d
	or	e
	jr	nz,ioc_gb_wait
	djnz	ioc_gb_outer
	pop	bc
	scf
	ret
ioc_gb_ready:
	in	a,(ZOS_SIO1B_DATA)
	pop	bc
	or	a
	ret

; ---------------------------------------------------------------------------
; Interrupt tokens (module-private; the kernel equivalents are not part of
; the driver API).  Standard Z80 pattern: LD A,I exposes IFF2 in P/V.
; ---------------------------------------------------------------------------

; uint8_t ioc_io_irq_save_disable(void);  A = L = 1 if IFF2 was set.
_ioc_io_irq_save_disable::
	ld	a,i
	di
	ld	l,#0
	jp	po,ioc_irq_was_off
	inc	l
ioc_irq_was_off:
	ld	a,l
	ret

; void ioc_io_irq_restore(uint8_t token);  A = token.
_ioc_io_irq_restore::
	or	a
	ret	z
	ei
	ret
