; -----------------------------------------------------------------------------
; KonaOS libkos: direct I/O on the held serial port (kos/serial.h).
;
; serial_hold (serial.c) stored the data and control ports KSIO_OP_INFO
; reported; these routines read RR0 and the data port themselves, with
; no call into the driver, because at 115,200 baud a byte arrives every
; 868 T-states and a driver call costs more.  A port of 0 means the port
; is not held (port 0 is the memory latch, never touched here).
;
; The Z80 SIO values: WR5 is register 5 (select 05h in WR0) and holds
; DTR, 8-bit transmit, transmit enable and RTS (D1); RR0 D0 is "a byte is
; received", D2 "the transmit buffer is empty".
;
; SDCC sdcccall(1): the first 8-bit argument in A, the first 16-bit in
; HL and the second in DE; a 16-bit result in DE.
; -----------------------------------------------------------------------------

	.module serial_io
	.globl _serial_rts
	.globl _serial_getc_wait
	.globl _serial_putc
	.globl _serial_write
	.globl _serial_data_port
	.globl _serial_ctrl_port

SERIAL_WR5_SELECT  = 0x05
SERIAL_WR5_RTS_ON  = 0xEA		; DTR, TX 8 bits, TX enable, RTS
SERIAL_WR5_RTS_OFF = 0xE8		; the same without RTS
SERIAL_RR0_TX_EMPTY = 0x04
; The receive poll below is 49 T-states, so 2041 polls are 10 ms at
; 10 MHz: one tick (SERIAL_TICKS_PER_SECOND = 100).
SERIAL_POLLS_PER_TICK = 2041

	.area _CODE

; void serial_rts(unsigned char on);  A = on
_serial_rts::
	ld	e,#SERIAL_WR5_RTS_OFF
	or	a
	jr	z,serial_rts_write
	ld	e,#SERIAL_WR5_RTS_ON
serial_rts_write:
	ld	a,(_serial_ctrl_port)
	or	a
	ret	z			; not held
	ld	c,a
	ld	a,#SERIAL_WR5_SELECT
	out	(c),a
	out	(c),e
	ret

; int serial_getc_wait(unsigned int ticks);  HL = ticks, DE = byte or -1
;   The first test is the hot path: during a stream a byte is usually
;   already waiting.
_serial_getc_wait::
	ld	a,(_serial_ctrl_port)
	ld	c,a
	or	a
	jr	z,serial_getc_none	; not held
	in	a,(c)
	rrca
	jr	c,serial_getc_hit
	ld	a,h
	or	l
	jr	z,serial_getc_none	; 0 ticks: one check only
serial_getc_tick:
	ld	de,#SERIAL_POLLS_PER_TICK
serial_getc_poll:
	in	a,(c)			; 12
	rrca				;  4
	jr	c,serial_getc_hit	;  7
	dec	de			;  6
	ld	a,d			;  4
	or	e			;  4
	jr	nz,serial_getc_poll	; 12
	dec	hl
	ld	a,h
	or	l
	jr	nz,serial_getc_tick
serial_getc_none:
	ld	de,#0xFFFF
	ret
serial_getc_hit:
	ld	a,(_serial_data_port)
	ld	c,a
	in	e,(c)
	ld	d,#0
	ret

; serial_tx (putc and write): B = the byte.  Waits for the transmit
; buffer, at most 65,536 polls of 52 T-states (about a third of a
; second); carry set when it never emptied or the port is not held.
; Preserves DE and HL.
serial_tx:
	ld	a,(_serial_ctrl_port)
	ld	c,a
	or	a
	scf
	ret	z			; not held
	push	de
	ld	de,#0
serial_tx_poll:
	in	a,(c)
	and	#SERIAL_RR0_TX_EMPTY
	jr	nz,serial_tx_ready
	dec	de
	ld	a,d
	or	e
	jr	nz,serial_tx_poll
	pop	de
	scf
	ret
serial_tx_ready:
	pop	de
	ld	a,(_serial_data_port)
	ld	c,a
	out	(c),b
	or	a			; carry clear: sent
	ret

; int serial_putc(unsigned char ch);  A = ch, DE = 0 or -1
_serial_putc::
	ld	b,a
	call	serial_tx
	ld	de,#0
	ret	nc
	dec	de
	ret

; int serial_write(const void *buffer, unsigned int length);
;   HL = buffer, DE = length; DE = the bytes sent.
_serial_write::
	push	de			; the length, for the count
serial_write_next:
	ld	a,d
	or	e
	jr	z,serial_write_done
	ld	b,(hl)
	call	serial_tx
	jr	c,serial_write_done
	inc	hl
	dec	de
	jr	serial_write_next
serial_write_done:
	pop	hl			; length - left = sent
	or	a
	sbc	hl,de
	ex	de,hl
	ret
