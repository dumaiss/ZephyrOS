; -----------------------------------------------------------------------------
; IOC two-lane port I/O and streaming loops (drivers/ioc, MOD module).
;
; All SIO1/B (command lane) and SIO1/A (bulk lane) access for the IOC class
; lives here.  The C side (ioc.c) owns framing, CRC, validation, and state;
; this file owns every `in`/`out` and the timing-critical byte loops.
;
; Shared command block passed by pointer in HL (sdcccall(1), single 16-bit
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
; Shared bulk block passed by pointer in HL:
;
;   struct ioc_bulk_block {
;       uint8_t *buf;     0..1  payload source/destination
;       uint16_t len;     2..3  payload byte count, 1..512
;       uint8_t  hunt;    4     read: 1 = Enter Hunt before the scan
;       uint8_t  header[5]; 5..9 LEN_LO LEN_HI TYPE SEQ STATUS
;   };
;   For a read the routine captures the header from the wire; for a write
;   the C side has already built it, bound to the READY's TYPE/sequence.
;
; Return values (sdcccall(1): 8-bit result in A):
;   ioc_io_wait_ready     0 = COMMAND_READY seen, 1 = bounded timeout
;   ioc_io_tx             0 = packet + trailers sent, RTS released, RX
;                            armed; 1 = byte timeout (RTS released)
;   ioc_io_rx             0 = raw frame captured; 1 = no first byte;
;                         2 = marker-scan byte timeout; 3 = scan budget
;                            exhausted; 4 = body byte timeout;
;                         5 = declared length out of range
;   ioc_io_bulk_read      0 = packet captured (header published);
;                         1 = bounded stall/timeout (RTS released);
;                         3 = marker-scan budget exhausted; 4 = /CTSA
;                            never deasserted after the RTS release
;   ioc_io_bulk_write     0 = sent and /CTSA deasserted; 1 = bounded
;                            stall (RTS released); 2 = sent but /CTSA
;                            never deasserted
;   ioc_io_status_sample  KOS_IOC_HID_DOORBELL_MASK or 0
;   ioc_io_irq_save_disable  previous IFF2 state (1 = enabled)
;
; Every wait is bounded by the verified platform budgets.  Interrupts: the C
; caller masks around the command transaction and restores the exact entry
; state; ioc_io_rx may re-enable them for the long MCU wait only when the
; entry state was enabled, and masks again before consuming a byte.  The
; bulk routines own their own save/mask and restore the exact entry IFF on
; every success and error path; the write routine restores them before the
; potentially long controller commit wait.
; -----------------------------------------------------------------------------

	.module ioc_io
	.include "hardware.inc"

	.globl _ioc_io_ctrl_write
	.globl _ioc_io_ctrl_read
	.globl _ioc_io_data_write
	.globl _ioc_io_data_read
	.globl _ioc_io_wait_ready
	.globl _ioc_io_tx
	.globl _ioc_io_rx
	.globl _ioc_io_bulk_init
	.globl _ioc_io_bulk_read
	.globl _ioc_io_bulk_write
	.globl _ioc_io_status_sample
	.globl _ioc_io_irq_save_disable
	.globl _ioc_io_irq_restore

	.area _CODE

; ---------------------------------------------------------------------------
; Raw port primitives.
; ---------------------------------------------------------------------------

; void ioc_io_ctrl_write(uint8_t value);  A = value.
_ioc_io_ctrl_write::
	out	(KOS_SIO1B_CTRL),a
	ret

; uint8_t ioc_io_ctrl_read(void);  A = L = RR0.
_ioc_io_ctrl_read::
	in	a,(KOS_SIO1B_CTRL)
	ld	l,a
	ret

; void ioc_io_data_write(uint8_t value);  A = value.
_ioc_io_data_write::
	out	(KOS_SIO1B_DATA),a
	ret

; uint8_t ioc_io_data_read(void);  A = L = one data byte.
_ioc_io_data_read::
	in	a,(KOS_SIO1B_DATA)
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
	ld	b,#KOS_IOC_CMD_READY_OUTER
ioc_ready_outer:
	ld	de,#KOS_IOC_CMD_READY_TIMEOUT
ioc_ready_wait:
	ld	a,#KOS_IOC_WR0_RESET_EXT_STATUS
	out	(KOS_SIO1B_CTRL),a
	in	a,(KOS_SIO1B_CTRL)
	and	#KOS_IOC_CMD_READY_MASK
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
	ld	a,#KOS_IOC_WR0_RESET_EXT_STATUS
	out	(KOS_SIO1B_CTRL),a
	in	a,(KOS_SIO1B_CTRL)
	and	#KOS_IOC_HID_DOORBELL_MASK
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
	out	(KOS_SIO1B_CTRL),a
	ld	a,#KOS_IOC_WR5_TX_RTS_OFF
	out	(KOS_SIO1B_CTRL),a

	; PRELOAD, THEN GRANT.  Nothing shifts until the MCU clocks, so the
	; preloaded A5 waits in the transmitter; asserting RTS against an
	; empty transmitter would emit FFh underrun fill first.
	ld	a,(de)
	ld	c,a
	call	ioc_put_byte
	jp	c,ioc_tx_fail

	; First byte is buffered: reset the Tx Underrun/EOM latch so the
	; transmitter leaves idle marking and shifts the preloaded byte.
	ld	a,#KOS_IOC_WR0_RESET_EOM
	out	(KOS_SIO1B_CTRL),a

	; Grant.  RTS is a held "unacknowledged request" level.
	ld	a,#0x05
	out	(KOS_SIO1B_CTRL),a
	ld	a,#KOS_IOC_WR5_TX_RTS_ON
	out	(KOS_SIO1B_CTRL),a

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
	ld	b,#KOS_IOC_SEND_TRAILER_BYTES
ioc_tx_trailer:
	ld	c,#KOS_IOC_SEND_TRAILER_FILL
	call	ioc_put_byte
	jp	c,ioc_tx_fail
	djnz	ioc_tx_trailer

	; Release RTS by disabling TX: an External-Sync transmitter never
	; reports empty, so clearing only the RTS bit would latch /RTS low.
	ld	a,#0x05
	out	(KOS_SIO1B_CTRL),a
	ld	a,#KOS_IOC_WR5_IDLE
	out	(KOS_SIO1B_CTRL),a

	; Arm the receiver before interrupts return.  Clear the error latch
	; without resetting the channel, then enable RX - Enter Hunt only
	; for the first establishment (HL still addresses io->hunt).
	ld	a,#KOS_IOC_WR0_RESET_ERROR
	out	(KOS_SIO1B_CTRL),a
	ld	a,(hl)
	or	a
	ld	b,#KOS_IOC_WR3_RX_NO_HUNT
	jr	z,ioc_tx_arm
	ld	b,#KOS_IOC_WR3_RX_HUNT
ioc_tx_arm:
	ld	a,#0x03
	out	(KOS_SIO1B_CTRL),a
	ld	a,b
	out	(KOS_SIO1B_CTRL),a
	xor	a
	ret

ioc_tx_fail:
	; Release RTS exactly as the success path does, then report the
	; bounded timeout.  The receiver is deliberately left alone: only a
	; channel reset, a receiver disable, or Enter Hunt ends character
	; assembly, so a partial request cannot destroy the boundary.
	ld	a,#0x05
	out	(KOS_SIO1B_CTRL),a
	ld	a,#KOS_IOC_WR5_IDLE
	out	(KOS_SIO1B_CTRL),a
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
	ld	a,#KOS_IOC_WR0_RESET_ERROR
	out	(KOS_SIO1B_CTRL),a

	; Drain at most the FIFO depth plus one stale full-duplex byte.
	ld	b,#KOS_IOC_RX_DRAIN_LIMIT
ioc_rx_drain:
	xor	a
	out	(KOS_SIO1B_CTRL),a
	in	a,(KOS_SIO1B_CTRL)
	and	#KOS_SIO_RR0_RX_AVAILABLE
	jr	z,ioc_rx_drained
	in	a,(KOS_SIO1B_DATA)
	djnz	ioc_rx_drain
ioc_rx_drained:
	ld	a,#KOS_IOC_WR0_RESET_ERROR
	out	(KOS_SIO1B_CTRL),a

	; Restore interrupts for the potentially long MCU reply wait when
	; the transaction entered with them enabled.
	pop	af
	or	a
	jr	z,ioc_rx_wait_first
	ei
ioc_rx_wait_first:
	ld	b,#KOS_IOC_CMD_BYTE_OUTER
ioc_rx_wait_outer:
	ld	de,#KOS_IOC_CMD_BYTE_TIMEOUT
ioc_rx_wait:
	in	a,(KOS_SIO1B_CTRL)
	and	#KOS_SIO_RR0_RX_AVAILABLE
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
	ld	b,#KOS_IOC_CMD_REPLY_SCAN_LIMIT
ioc_rx_scan0:
	call	ioc_get_byte
	jr	c,ioc_rx_marker_timeout
	cp	#KOS_IOC_PACKET_SYNC0
	jr	z,ioc_rx_scan1
	djnz	ioc_rx_scan0
	jr	ioc_rx_scan_fail
ioc_rx_scan1:
	call	ioc_get_byte
	jr	c,ioc_rx_marker_timeout
	cp	#KOS_IOC_PACKET_SYNC1
	jr	z,ioc_rx_header
	cp	#KOS_IOC_PACKET_SYNC0
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
	cp	#KOS_IOC_PACKET_FIXED_LEN
	jr	c,ioc_rx_bad_header
	cp	#(KOS_IOC_PACKET_FIXED_LEN + KOS_IOC_CMD_MAX_DATA + 1)
	jr	nc,ioc_rx_bad_header
	sub	#KOS_IOC_PACKET_FIXED_LEN
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
	ld	b,#KOS_IOC_CMD_BYTE_OUTER
ioc_pb_outer:
	ld	de,#KOS_IOC_CMD_BYTE_TIMEOUT
ioc_pb_wait:
	in	a,(KOS_SIO1B_CTRL)
	and	#KOS_SIO_RR0_TX_EMPTY
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
	out	(KOS_SIO1B_DATA),a
	pop	de
	or	a
	ret

; Out: carry set on timeout; else A = received byte.  Clobbers AF, DE.
ioc_get_byte:
	push	bc
	ld	b,#KOS_IOC_CMD_BYTE_OUTER
ioc_gb_outer:
	ld	de,#KOS_IOC_CMD_BYTE_TIMEOUT
ioc_gb_wait:
	in	a,(KOS_SIO1B_CTRL)
	and	#KOS_SIO_RR0_RX_AVAILABLE
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
	in	a,(KOS_SIO1B_DATA)
	pop	bc
	or	a
	ret

; ---------------------------------------------------------------------------
; Interrupt tokens (module-private; the kernel equivalents are not part of
; the driver API).  Standard Z80 pattern: LD A,I exposes IFF2 in P/V.
; ---------------------------------------------------------------------------

; uint8_t ioc_io_irq_save_disable(void);  A = L = 1 if IFF2 was set.
_ioc_io_irq_save_disable::
	; IFF2 is read up to three times: on the board an interrupt
	; accepted right after LD A,I makes that read report 0 (the
	; documented LD A,I behaviour), and the interrupt's own EI/RETI
	; makes the next read true.
	ld	a,i
	jp	pe,ioc_io_iff1
	ld	a,i
	jp	pe,ioc_io_iff1
	ld	a,i
ioc_io_iff1:
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

; ===========================================================================
; SIO1/A bulk lane (ports 30h/31h).
;
; Ported from the proven IOCBULK/IOCBULKW discipline of the deployed host
; transport.  The deployed pair bypasses bulk DATA CRC on both ends: the
; read path consumes the two trailer bytes and never compares them, while
; the write path still arms the SIO TX CRC generator and issues the
; terminating underrun so the wire/timing behavior stays identical.
; Marker, header, admission and state validation are never bypassed.
; ===========================================================================

; ---------------------------------------------------------------------------
; void ioc_io_bulk_init(void);
;   One-time SIO1/A programming: channel reset, WR1 interrupts/WAIT off,
;   External Sync x1, AAh fill on WR6/WR7, receiver enabled without Hunt,
;   transmitter disabled and RTS released.  Never called again: only a
;   channel reset, a receiver disable, or Enter Hunt ends character
;   assembly.
; ---------------------------------------------------------------------------
_ioc_io_bulk_init::
	ld	a,#KOS_IOC_WR0_RESET_CHANNEL
	out	(KOS_SIO1A_CTRL),a

	ld	a,#KOS_SIO_WR_SELECT_WR1
	out	(KOS_SIO1A_CTRL),a
	ld	a,#KOS_IOC_WR1_INT_WAIT_OFF
	out	(KOS_SIO1A_CTRL),a

	ld	a,#KOS_SIO_WR_SELECT_WR4
	out	(KOS_SIO1A_CTRL),a
	ld	a,#KOS_IOC_WR4_EXTSYNC_X1
	out	(KOS_SIO1A_CTRL),a

	ld	a,#KOS_IOC_WR_SELECT_WR6
	out	(KOS_SIO1A_CTRL),a
	ld	a,#KOS_IOC_BULK_SYNC_FILL
	out	(KOS_SIO1A_CTRL),a

	ld	a,#KOS_IOC_WR_SELECT_WR7
	out	(KOS_SIO1A_CTRL),a
	ld	a,#KOS_IOC_BULK_SYNC_FILL
	out	(KOS_SIO1A_CTRL),a

	ld	a,#KOS_SIO_WR_SELECT_WR3
	out	(KOS_SIO1A_CTRL),a
	ld	a,#KOS_IOC_WR3_BULK_RX_NO_HUNT
	out	(KOS_SIO1A_CTRL),a

	ld	a,#KOS_SIO_WR_SELECT_WR5
	out	(KOS_SIO1A_CTRL),a
	ld	a,#KOS_IOC_WR5_BULK_IDLE
	out	(KOS_SIO1A_CTRL),a
	ret

; ---------------------------------------------------------------------------
; uint8_t ioc_io_bulk_read(struct ioc_bulk_block *io);  HL = block.
;   Receive one admitted bulk packet.  Interrupts are masked for the whole
;   transfer (the controller is clock master and never waits) and the exact
;   entry IFF is restored on every exit.  Enter Hunt only while io->hunt
;   says the boundary is not established yet.  On success the captured
;   five-byte header is published into io->header for C-side validation.
;   Out: A = 0 captured, 1 bounded timeout, 3 marker scan exhausted,
;   4 /CTSA never deasserted.  Clobbers AF, BC, DE, HL.
; ---------------------------------------------------------------------------
_ioc_io_bulk_read::
	; Preserve the block pointer before the IRQ helper: that helper
	; returns the token in A and L (the standard Z80 pattern), so it
	; clobbers HL.
	ld	(_ioc_bulk_block),hl
	call	_ioc_io_irq_save_disable
	ld	(_ioc_bulk_token),a
	ld	hl,(_ioc_bulk_block)

	; Copy the inputs into module scratch so the byte loops can use
	; every register.
	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	inc	hl			; block + 2 = len low
	ld	(_ioc_bulk_buf),de
	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	inc	hl			; block + 4 = hunt
	ld	(_ioc_bulk_len),de
	ld	a,(hl)
	ld	(_ioc_bulk_hunt),a

	; Clear the error latch without resetting the channel.
	ld	a,#KOS_IOC_WR0_RESET_ERROR
	out	(KOS_SIO1A_CTRL),a

	; Receiver: Hunt is used only until the first verified bulk packet.
	ld	a,#KOS_SIO_WR_SELECT_WR3
	out	(KOS_SIO1A_CTRL),a
	ld	a,(_ioc_bulk_hunt)
	or	a
	ld	a,#KOS_IOC_WR3_BULK_RX_NO_HUNT
	jr	z,ioc_br_wr3
	ld	a,#KOS_IOC_WR3_BULK_RX_HUNT
ioc_br_wr3:
	out	(KOS_SIO1A_CTRL),a

	; Drain at most the FIFO depth plus one stale full-duplex byte.
	; Unbounded, this races the reply and swallows its first byte; the
	; remainder is drained at the head of the next transfer.
	ld	b,#KOS_IOC_RX_DRAIN_LIMIT
ioc_br_drain:
	in	a,(KOS_SIO1A_CTRL)
	and	#KOS_SIO_RR0_RX_AVAILABLE
	jr	z,ioc_br_drained
	in	a,(KOS_SIO1A_DATA)
	djnz	ioc_br_drain
ioc_br_drained:

	; Tell the controller we are in the read loop.  It is blocked on
	; this; RTS is a held level, not an edge.
	ld	a,#0x05
	out	(KOS_SIO1A_CTRL),a
	ld	a,#KOS_IOC_WR5_BULK_RTS_ON
	out	(KOS_SIO1A_CTRL),a

	; RX admission.  Auto Enables is off, so /DCDA is a software level:
	; the controller asserts it, waits, and only then opens the clock
	; gate, so no bit can arrive before this poll succeeds.
	ld	bc,#0
ioc_br_dcd_wait:
	ld	a,#KOS_IOC_WR0_RESET_EXT_STATUS
	out	(KOS_SIO1A_CTRL),a
	in	a,(KOS_SIO1A_CTRL)
	and	#KOS_SIO_RR0_DCD
	jr	nz,ioc_br_admitted
	dec	bc
	ld	a,b
	or	c
	jr	nz,ioc_br_dcd_wait
	jp	ioc_br_timeout
ioc_br_admitted:

	; Locate the complete A5 5A marker.  A trailing fill byte from the
	; preceding transaction can become FIFO-visible only under these
	; clocks, so the first visible byte is not necessarily a frame start.
	ld	b,#KOS_IOC_BULK_SCAN_LIMIT
ioc_br_scan0:
	call	ioc_bulk_get
	jp	c,ioc_br_timeout
	cp	#KOS_IOC_PACKET_SYNC0
	jr	z,ioc_br_scan1
	djnz	ioc_br_scan0
	jp	ioc_br_marker_fail
ioc_br_scan1:
	call	ioc_bulk_get
	jp	c,ioc_br_timeout
	cp	#KOS_IOC_PACKET_SYNC1
	jr	z,ioc_br_header
	cp	#KOS_IOC_PACKET_SYNC0
	jr	z,ioc_br_scan1		; overlapping A5 A5 5A
	djnz	ioc_br_scan0
	jp	ioc_br_marker_fail

	; Capture all five header bytes with the same INI-shaped loop as the
	; payload: the controller is already streaming at the wire rate and
	; five call/return pairs would overrun the three-byte FIFO.
ioc_br_header:
	ld	hl,#_ioc_bulk_hdr
	ld	b,#5
	ld	c,#KOS_SIO1A_DATA
	ld	de,#0
ioc_br_hdr_wait:
	in	a,(KOS_SIO1A_CTRL)
	and	#KOS_SIO_RR0_RX_AVAILABLE
	jr	nz,ioc_br_hdr_ready
	dec	de
	ld	a,d
	or	e
	jr	nz,ioc_br_hdr_wait
	jp	ioc_br_timeout
ioc_br_hdr_ready:
	ini
	jp	nz,ioc_br_hdr_wait

	; Payload: the admitted (READY-authoritative) length, never a length
	; taken from the wire.  INI forces the register budget: B is the
	; counter, C the port, HL the destination; DE is a whole-transfer
	; stall budget and the 256-byte block count lives on the stack.
	ld	hl,(_ioc_bulk_buf)
	ld	de,(_ioc_bulk_len)
	ld	a,d
	ld	b,e
	push	af
	ld	c,#KOS_SIO1A_DATA
	ld	de,#0
	ld	a,b
	or	a
	jr	nz,ioc_br_poll
ioc_br_chunk:
	pop	af
	or	a
	jr	z,ioc_br_trailer	; popped, not re-pushed: stack balanced
	dec	a
	push	af
	ld	b,#0			; B = 0 means 256 bytes
ioc_br_poll:
	in	a,(KOS_SIO1A_CTRL)
	and	#KOS_SIO_RR0_RX_AVAILABLE
	jr	nz,ioc_br_got
	dec	de
	ld	a,d
	or	e
	jr	nz,ioc_br_poll
	pop	af
	jp	ioc_br_timeout
ioc_br_got:
	ini
	jp	nz,ioc_br_poll
	jr	ioc_br_chunk

	; Two trailer bytes follow the payload.  The deployed pair bypasses
	; bulk DATA CRC: the bytes are consumed so the next transfer starts
	; clean, and deliberately never compared.
ioc_br_trailer:
	call	ioc_bulk_get
	jp	c,ioc_br_timeout
	call	ioc_bulk_get
	jp	c,ioc_br_timeout

	; All bytes in: release RTS, then confirm the controller ended the
	; phase (it holds /CTSA for the duration of the transfer).
	call	ioc_bulk_rts_off
	ld	de,#0
ioc_br_cts_wait:
	ld	a,#KOS_IOC_WR0_RESET_EXT_STATUS
	out	(KOS_SIO1A_CTRL),a
	in	a,(KOS_SIO1A_CTRL)
	and	#KOS_SIO_RR0_CTS
	jr	z,ioc_br_ok
	dec	de
	ld	a,d
	or	e
	jr	nz,ioc_br_cts_wait
	call	ioc_bulk_irq_restore
	ld	a,#4
	ret

ioc_br_ok:
	; Publish the captured header for the C-side validation.
	ld	hl,(_ioc_bulk_block)
	ld	de,#5
	add	hl,de
	ex	de,hl
	ld	hl,#_ioc_bulk_hdr
	ld	bc,#5
	ldir
	call	ioc_bulk_irq_restore
	xor	a
	ret

ioc_br_marker_fail:
	call	ioc_bulk_rts_off
	call	ioc_bulk_irq_restore
	ld	a,#3
	ret

ioc_br_timeout:
	call	ioc_bulk_rts_off
	call	ioc_bulk_irq_restore
	ld	a,#1
	ret

; ---------------------------------------------------------------------------
; uint8_t ioc_io_bulk_write(struct ioc_bulk_block *io);  HL = block.
;   Transmit one admitted bulk packet.  Interrupts are masked across the
;   unpausable stream and restored before the potentially long controller
;   commit wait; every exit restores the exact entry IFF.
;   Out: A = 0 sent and /CTSA deasserted, 1 bounded stall (RTS released),
;   2 sent but /CTSA never deasserted.  Clobbers AF, BC, DE, HL.
; ---------------------------------------------------------------------------
_ioc_io_bulk_write::
	; Preserve the block pointer before the IRQ helper: that helper
	; returns the token in A and L (the standard Z80 pattern), so it
	; clobbers HL.
	ld	(_ioc_bulk_block),hl
	call	_ioc_io_irq_save_disable
	ld	(_ioc_bulk_token),a
	ld	hl,(_ioc_bulk_block)

	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	inc	hl			; block + 2 = len low
	ld	(_ioc_bulk_buf),de
	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	ld	(_ioc_bulk_len),de

	; Arm channel A for transmit without touching the persistent RX
	; boundary: WR3 C1 keeps the receiver enabled.  The full-duplex
	; idle bytes received while we send are drained by the next read.
	ld	a,#KOS_IOC_WR0_RESET_ERROR
	out	(KOS_SIO1A_CTRL),a
	ld	a,#KOS_SIO_WR_SELECT_WR3
	out	(KOS_SIO1A_CTRL),a
	ld	a,#KOS_IOC_WR3_BULK_TX
	out	(KOS_SIO1A_CTRL),a

	; Fill of its own (AAh), so a trailer byte is never mistaken for a
	; zeroed CRC register.
	ld	a,#KOS_IOC_WR_SELECT_WR6
	out	(KOS_SIO1A_CTRL),a
	ld	a,#KOS_IOC_BULK_SYNC_FILL
	out	(KOS_SIO1A_CTRL),a
	ld	a,#KOS_IOC_WR_SELECT_WR7
	out	(KOS_SIO1A_CTRL),a
	ld	a,#KOS_IOC_BULK_SYNC_FILL
	out	(KOS_SIO1A_CTRL),a

	; Transmitter enabled, RTS still off.
	ld	a,#0x05
	out	(KOS_SIO1A_CTRL),a
	ld	a,#KOS_IOC_WR5_BULK_RTS_OFF
	out	(KOS_SIO1A_CTRL),a

	; Arm the TX CRC generator before the first byte so its coverage is
	; the whole transmission; the sacrificial lead-in is inside it.
	ld	a,#KOS_IOC_WR0_RESET_TX_CRC
	out	(KOS_SIO1A_CTRL),a

	; Stage the lead-in BEFORE RTS.  RTS is the controller's "go":
	; whatever sits in the transmitter at that moment goes on the wire,
	; and an empty transmitter would emit fill first.
	ld	a,#KOS_IOC_BULK_LEADIN
	call	ioc_bulk_put
	jp	c,ioc_bw_stall

	; Grant.
	ld	a,#0x05
	out	(KOS_SIO1A_CTRL),a
	ld	a,#KOS_IOC_WR5_BULK_RTS_ON
	out	(KOS_SIO1A_CTRL),a

	; TX admission.  Auto Enables is off: /CTSA is sampled as a level
	; and the clock gate stays closed until the controller is ready.
	ld	de,#0
ioc_bw_cts_wait:
	ld	a,#KOS_IOC_WR0_RESET_EXT_STATUS
	out	(KOS_SIO1A_CTRL),a
	in	a,(KOS_SIO1A_CTRL)
	and	#KOS_SIO_RR0_CTS
	jr	nz,ioc_bw_admitted
	dec	de
	ld	a,d
	or	e
	jr	nz,ioc_bw_cts_wait
	jp	ioc_bw_stall
ioc_bw_admitted:

	; Marker, then the five-byte header the C side built for this READY.
	ld	a,#KOS_IOC_PACKET_SYNC0
	call	ioc_bulk_put
	jp	c,ioc_bw_stall
	ld	a,#KOS_IOC_PACKET_SYNC1
	call	ioc_bulk_put
	jp	c,ioc_bw_stall
	ld	hl,(_ioc_bulk_block)
	ld	de,#5
	add	hl,de			; -> header[0]
	ld	b,#5
ioc_bw_header:
	ld	a,(hl)
	call	ioc_bulk_put
	jp	c,ioc_bw_stall
	inc	hl
	djnz	ioc_bw_header

	; Payload, OUTI-shaped, the same 56-T-state budget as the read loop.
	ld	hl,(_ioc_bulk_buf)
	ld	de,(_ioc_bulk_len)
	ld	a,d
	ld	b,e
	push	af
	ld	c,#KOS_SIO1A_DATA
	ld	de,#0
	ld	a,b
	or	a
	jp	nz,ioc_bw_poll
ioc_bw_chunk:
	pop	af
	or	a
	jp	z,ioc_bw_sent		; popped, not re-pushed: stack balanced
	dec	a
	push	af
	ld	b,#0
ioc_bw_poll:
	in	a,(KOS_SIO1A_CTRL)
	and	#KOS_SIO_TX_READY
	jr	nz,ioc_bw_got
	dec	de
	ld	a,d
	or	e
	jr	nz,ioc_bw_poll
	pop	af
	jp	ioc_bw_stall
ioc_bw_got:
	outi
	jp	nz,ioc_bw_poll
	jr	ioc_bw_chunk

ioc_bw_sent:
	; The EOM reset is issued only now, after the final payload byte.
	; With the latch reset, an underrun makes the SIO send the
	; accumulated CRC in place of fill; issued earlier it would have
	; made the CTS-admission underrun emit the CRC.  Nothing may be
	; written to the data port after this.
	ld	a,#KOS_IOC_WR0_RESET_EOM
	out	(KOS_SIO1A_CTRL),a

	; Wait for the transmitter to run dry: RR0 D6 is the underrun flag
	; and marks the moment the CRC starts going out.
	ld	de,#0
ioc_bw_eom_wait:
	in	a,(KOS_SIO1A_CTRL)
	and	#KOS_SIO_RR0_TX_UNDERRUN
	jr	nz,ioc_bw_eom_done
	dec	de
	ld	a,d
	or	e
	jr	nz,ioc_bw_eom_wait
	jp	ioc_bw_stall
ioc_bw_eom_done:

	; Measured guard for the two CRC characters to clear the shift
	; register.  Counted, because RR1's All Sent reads "always 1" in
	; sync mode.  128 iterations is five times the requirement.
	ld	de,#KOS_IOC_BULK_UNDERRUN_GUARD
ioc_bw_crc_out:
	dec	de
	ld	a,d
	or	e
	jr	nz,ioc_bw_crc_out

	; Release RTS by disabling TX (which lets the CRC sequence
	; complete), then restore interrupts BEFORE the controller commit
	; wait: every byte is already out and this wait can span card
	; programming time, so it must not hold the machine masked.
	call	ioc_bulk_rts_off
	call	ioc_bulk_irq_restore
	ld	de,#0
ioc_bw_cts_out:
	ld	a,#KOS_IOC_WR0_RESET_EXT_STATUS
	out	(KOS_SIO1A_CTRL),a
	in	a,(KOS_SIO1A_CTRL)
	and	#KOS_SIO_RR0_CTS
	jr	z,ioc_bw_ok
	dec	de
	ld	a,d
	or	e
	jr	nz,ioc_bw_cts_out
	ld	a,#2
	ret
ioc_bw_ok:
	xor	a
	ret

ioc_bw_stall:
	call	ioc_bulk_rts_off
	call	ioc_bulk_irq_restore
	ld	a,#1
	ret

; ---------------------------------------------------------------------------
; Bulk-internal helpers.
; ---------------------------------------------------------------------------

; Release RTS on SIO1/A.  In External Sync an enabled transmitter streams
; fill on underrun and may never satisfy transmit-empty, so releasing RTS
; means disabling the transmitter.  WR5 does not touch the receiver or its
; character boundary.
ioc_bulk_rts_off:
	ld	a,#0x05
	out	(KOS_SIO1A_CTRL),a
	ld	a,#KOS_IOC_WR5_BULK_IDLE
	out	(KOS_SIO1A_CTRL),a
	ret

; In: A = byte.  Out: carry set on the bounded timeout.  Clobbers AF, DE;
; preserves BC and HL.
ioc_bulk_put:
	push	bc
	ld	c,a
	ld	de,#0
ioc_bput_wait:
	in	a,(KOS_SIO1A_CTRL)
	and	#KOS_SIO_TX_READY
	jr	nz,ioc_bput_ready
	dec	de
	ld	a,d
	or	e
	jr	nz,ioc_bput_wait
	pop	bc
	scf
	ret
ioc_bput_ready:
	ld	a,c
	out	(KOS_SIO1A_DATA),a
	pop	bc
	or	a
	ret

; Out: carry set on the bounded timeout; else A = received byte.
; Clobbers AF, DE.
ioc_bulk_get:
	ld	de,#0
ioc_bget_wait:
	in	a,(KOS_SIO1A_CTRL)
	and	#KOS_SIO_RR0_RX_AVAILABLE
	jr	nz,ioc_bget_ready
	dec	de
	ld	a,d
	or	e
	jr	nz,ioc_bget_wait
	scf
	ret
ioc_bget_ready:
	in	a,(KOS_SIO1A_DATA)
	or	a
	ret

; Restore the exact entry IFF for a bulk transfer (token in module
; scratch) and return to the caller of the bulk routine.
ioc_bulk_irq_restore:
	ld	a,(_ioc_bulk_token)
	jp	_ioc_io_irq_restore

; ---------------------------------------------------------------------------
; Bulk-transport scratch.  Module BSS (_DATA): zeroed by the loader and
; relocated with the module image.  Single IOC instance, single foreground
; transfer, so these are not re-entrant by design (the C busy guard and the
; admission state serialize every operation).
; ---------------------------------------------------------------------------
	.area _DRIVER_IO
_ioc_bulk_block:	.ds 2
_ioc_bulk_buf:		.ds 2
_ioc_bulk_len:		.ds 2
_ioc_bulk_hunt:		.ds 1
_ioc_bulk_token:	.ds 1
_ioc_bulk_hdr:		.ds 5
