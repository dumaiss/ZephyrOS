; -----------------------------------------------------------------------------
; Zephyr-80 Mk2 boot proof image (test-only; tests/test_mk2_boot.py).
;
; The boot sequence of architecture 2.7.5 on the LS612 DAT board, with no
; nucleus code:
;
;   ROM page 0, pass mode (logical = physical ROM 0000h-FFFFh, no RAM):
;     1. DI; program all sixteen DAT entries from `dat_image` with
;        OUT (C),A and B = A: the page on D0-D6 and A8-A14 (I/O A15 = 0,
;        pages are below 80h), whichever the entry inputs are wired to.
;        No stack exists yet: nothing here pushes, calls or writes
;        memory.
;     2. MAP_ENABLE with DAT_BANK = 0.  Entry 0 maps ROM page 00h, so the
;        bytes at 0000h-1FFFh are the same in both views and the
;        instruction after the OUT is fetched from the same ROM byte.
;   ROM page 0, mapped, system DAT:
;     3. Copy the payload from its ROM page (07h, seen at 2000h through
;        entry 1) into the SRAM common page (entries 7 and 15, E000h).
;     4. JP into the payload.
;   Common page (SRAM), mapped:
;     5. SP at the top of the common page; write the software mirror of
;        the entries and the control byte (no RAM existed when the OUTs
;        ran; the IOC guarantees no NMI before this point).
;     6. Early console (SIO0/B, architecture 2.6), banner with the boot
;        count kept in the common page.
;     7. DAT_BANK system -> working -> system -> working -> system: print
;        the ROM check string at 6000h and the SRAM check byte at 8000h
;        read through each map.  The mirror is written before each OUT.
;     8. First boot: CMD_RESET (02h) over the IOC command lane (SIO1/B);
;        the IOC drives the Z80 reset and the board returns to pass mode
;        at 0000h.  Second boot: `done`, HALT.
;
; Common page layout (SRAM page 7Fh, logical E000h-FFFFh):
;   E000h-E3FFh   payload code and strings (copied from ROM page 07h)
;   FE00h-FE0Fh   software mirror of DAT entries 0-15
;   FE10h         software mirror of the control byte
;   FE20h-FE21h   boot magic, FE22h boot count (kept across a reset)
;   FF00h-FFFFh   stack (SP starts at 0000h: the first push is FFFEh)
; -----------------------------------------------------------------------------

	.module	mk2boot
	.include "hardware.inc"

; The mapper (architecture 2.7.4).
MK2_CONTROL		= 0x800F	; BC for OUT (C),A: A15 = 1, low 0Fh
MK2_MAPPED_SYSTEM	= 0x02		; MAP_ENABLE, DAT_BANK = 0
MK2_MAPPED_WORKING	= 0x03		; MAP_ENABLE, DAT_BANK = 1
MK2_ENTRIES		= 16

; Logical windows the image uses.
MK2_PAYLOAD_WINDOW	= 0x2000	; entry 1: the payload's ROM page
MK2_CHECK_ROM		= 0x6000	; entries 3 / 11: ROM check strings
MK2_CHECK_RAM		= 0x8000	; entries 4 / 12: SRAM check bytes

; Common-page state (not copied; survives a reset).
dat_mirror		= 0xFE00
ctl_mirror		= 0xFE10
boot_magic		= 0xFE20
boot_count		= 0xFE22
MK2_STACK_TOP		= 0x0000
BOOT_MAGIC		= 0x324B	; "K2"

; The IOC command lane: CMD_RESET (Zephyr-80 Code/MCU/IOController
; include/ioc_frame.h; src/handlers.c handler_reset).
IOC_CMD_RESET		= 0x02

	.area	_MK2BOOT (ABS)

; ---- ROM page 0: reset view and the low vectors of both DATs ---------------

	.org	0x0000
reset:
	di
	jp	boot

	.org	0x0038
rst38:					; IM 1 is never enabled here
	ret

	.org	0x0066
nmi:					; the IOC starts no RT clock here
	retn

	.org	0x0100
boot:
	ld	hl,#dat_image
	ld	c,#0			; C = entry 0
boot_entry:
	ld	a,(hl)
	ld	b,a			; B = A: wiring-agnostic, A15 = 0
	out	(c),a
	inc	hl
	inc	c
	ld	a,c
	cp	#MK2_ENTRIES
	jr	nz,boot_entry

	ld	bc,#MK2_CONTROL
	ld	a,#MK2_MAPPED_SYSTEM
	out	(c),a
boot_mapped::				; first instruction under the system DAT
	ld	hl,#MK2_PAYLOAD_WINDOW
	ld	de,#payload
	ld	bc,#payload_end - payload
	ldir
	jp	payload

; Entries 0-7: the system DAT; 8-15: the working DAT.  Page 00h-3Fh is
; ROM, 40h-7Fh SRAM; 7Fh is the common page of both.
dat_image::
	.db	0x00			; 0  0000h ROM page 0: code, vectors
	.db	0x07			; 1  2000h ROM page 7: the payload
	.db	0x40			; 2  4000h SRAM
	.db	0x05			; 3  6000h ROM page 5: "SYS-05"
	.db	0x41			; 4  8000h SRAM check page (system)
	.db	0x43			; 5  A000h SRAM
	.db	0x44			; 6  C000h SRAM
	.db	0x7F			; 7  E000h SRAM common page
	.db	0x00			; 8  0000h ROM page 0: vectors
	.db	0x45			; 9  2000h SRAM
	.db	0x46			; 10 4000h SRAM
	.db	0x06			; 11 6000h ROM page 6: "WRK-06"
	.db	0x42			; 12 8000h SRAM check page (working)
	.db	0x47			; 13 A000h SRAM
	.db	0x48			; 14 C000h SRAM
	.db	0x7F			; 15 E000h SRAM common page

; ---- ROM pages 5 and 6: one check string per map --------------------------

	.org	0xA000
	.ascii	"SYS-05"
	.db	0

	.org	0xC000
	.ascii	"WRK-06"
	.db	0

; ---- ROM page 7: the payload, linked at the common page --------------------

	.org	0xE000
payload::
	ld	sp,#MK2_STACK_TOP

	ld	hl,#dat_image		; ROM page 0 is still at 0000h
	ld	de,#dat_mirror
	ld	bc,#MK2_ENTRIES
	ldir
	ld	a,#MK2_MAPPED_SYSTEM
	ld	(ctl_mirror),a

	ld	hl,(boot_magic)
	ld	de,#BOOT_MAGIC
	or	a
	sbc	hl,de
	jr	z,payload_counted
	ld	(boot_magic),de
	xor	a
	ld	(boot_count),a
payload_counted:
	ld	hl,#boot_count
	inc	(hl)

	call	con_init
banner_start::
	ld	hl,#txt_banner
	call	con_puts
	ld	a,(boot_count)
	call	con_hex
	call	con_crlf
banner_end::

	; The DAT check: each value is read through the map it names.
	ld	a,(boot_count)
	add	a,#0x50
	ld	(MK2_CHECK_RAM),a	; system: SRAM page 41h
	ld	hl,#txt_sys
	call	con_puts
	ld	hl,#MK2_CHECK_ROM
	call	con_puts

	ld	a,#MK2_MAPPED_WORKING
	call	set_control
	ld	a,(boot_count)
	add	a,#0xA0
	ld	(MK2_CHECK_RAM),a	; working: SRAM page 42h
	ld	hl,#txt_wrk
	call	con_puts
	ld	hl,#MK2_CHECK_ROM
	call	con_puts

	ld	a,#MK2_MAPPED_SYSTEM
	call	set_control
	ld	hl,#txt_sys_again
	call	con_puts
	ld	hl,#MK2_CHECK_ROM
	call	con_puts
	ld	hl,#txt_ram_sys
	call	con_puts
	ld	a,(MK2_CHECK_RAM)
	call	con_hex

	ld	a,#MK2_MAPPED_WORKING
	call	set_control
	ld	hl,#txt_ram_wrk
	call	con_puts
	ld	a,(MK2_CHECK_RAM)
	call	con_hex
	ld	a,#MK2_MAPPED_SYSTEM
	call	set_control
	call	con_crlf

	ld	a,(boot_count)
	cp	#2
	jr	nc,payload_done
	ld	hl,#txt_reboot
	call	con_puts
	call	ioc_reset		; returns only when no reset came
	ld	hl,#txt_reboot_failed
	call	con_puts
payload_done:
	ld	hl,#txt_done
	call	con_puts
	halt

; A = control byte.  The mirror first, then the OUT (architecture 2.7.6).
set_control:
	ld	(ctl_mirror),a
	ld	bc,#MK2_CONTROL
	out	(c),a
	ret

; ---- early console: SIO0/B, polled (architecture 2.6) ----------------------

con_init:
	ld	hl,#con_init_seq
	ld	b,#con_init_end - con_init_seq
	ld	c,#KOS_SIO0B_CTRL
	otir
	ret

; A = character.  Bounded wait for TX-empty; on timeout the byte is dropped.
con_putc:
	push	de
	push	af
	ld	de,#0
con_putc_wait:
	in	a,(KOS_SIO0B_CTRL)
	and	#KOS_SIO_TX_READY
	jr	nz,con_putc_ready
	dec	de
	ld	a,d
	or	e
	jr	nz,con_putc_wait
	pop	af
	pop	de
	ret
con_putc_ready:
	pop	af
	out	(KOS_SIO0B_DATA),a
	pop	de
	ret

; HL = NUL-terminated string.
con_puts:
	ld	a,(hl)
	or	a
	ret	z
	call	con_putc
	inc	hl
	jr	con_puts

con_crlf:
	ld	a,#0x0D
	call	con_putc
	ld	a,#0x0A
	jr	con_putc

; A = byte, printed as two hex digits.
con_hex:
	push	af
	rrca
	rrca
	rrca
	rrca
	call	con_nibble
	pop	af
con_nibble:
	and	#0x0F
	add	a,#0x30			; '0'
	cp	#0x3A
	jr	c,con_putc
	add	a,#0x07			; 'A' - '9' - 1
	jr	con_putc

; ---- the IOC reset request (SIO1/B command lane) ---------------------------
; The lane programming of drivers/ioc/ioc_boot.c ioc_hw_init, the
; COMMAND_READY wait and the request order of drivers/ioc/ioc_io.asm
; ioc_io_tx (preload A5, EOM reset, RTS, body, two FFh trailers, release
; RTS by disabling TX).  CMD_RESET has no reply: the controller drives the
; Z80 reset.  Returns on a COMMAND_READY timeout or when no reset arrives.

ioc_reset:
	ld	hl,#ioc_init_seq
	ld	b,#ioc_init_end - ioc_init_seq
	ld	c,#KOS_SIO1B_CTRL
	otir

	ld	de,#0
ioc_reset_ready:
	ld	a,#KOS_IOC_WR0_RESET_EXT_STATUS
	out	(KOS_SIO1B_CTRL),a
	in	a,(KOS_SIO1B_CTRL)
	and	#KOS_IOC_CMD_READY_MASK
	jr	nz,ioc_reset_send
	dec	de
	ld	a,d
	or	e
	jr	nz,ioc_reset_ready
	ret

ioc_reset_send:
	ld	hl,#reset_frame
	ld	a,(hl)
	call	ioc_put			; preload A5
	ld	a,#KOS_IOC_WR0_RESET_EOM
	out	(KOS_SIO1B_CTRL),a
	ld	a,#KOS_SIO_WR_SELECT_WR5
	out	(KOS_SIO1B_CTRL),a
	ld	a,#KOS_IOC_WR5_TX_RTS_ON
	out	(KOS_SIO1B_CTRL),a
	inc	hl
	ld	b,#reset_frame_end - reset_frame - 1
ioc_reset_body:
	ld	a,(hl)
	call	ioc_put
	inc	hl
	djnz	ioc_reset_body
	ld	b,#KOS_IOC_SEND_TRAILER_BYTES
ioc_reset_trailer:
	ld	a,#KOS_IOC_SEND_TRAILER_FILL
	call	ioc_put
	djnz	ioc_reset_trailer
	ld	a,#KOS_SIO_WR_SELECT_WR5
	out	(KOS_SIO1B_CTRL),a
	ld	a,#KOS_IOC_WR5_IDLE
	out	(KOS_SIO1B_CTRL),a	; RTS released: the request is complete

	ld	de,#0			; the controller holds the reset from here
ioc_reset_wait:
	dec	de
	ld	a,d
	or	e
	jr	nz,ioc_reset_wait
	ret

; A = byte.  Bounded wait for TX-empty on SIO1/B.
ioc_put:
	push	de
	push	af
	ld	de,#0
ioc_put_wait:
	in	a,(KOS_SIO1B_CTRL)
	and	#KOS_SIO_RR0_TX_EMPTY
	jr	nz,ioc_put_ready
	dec	de
	ld	a,d
	or	e
	jr	nz,ioc_put_wait
ioc_put_ready:
	pop	af
	out	(KOS_SIO1B_DATA),a
	pop	de
	ret

; ---- payload data ----------------------------------------------------------

con_init_seq:
	.db	KOS_SIO_WR0_CHANNEL_RESET
	.db	KOS_SIO_WR_SELECT_WR4, KOS_SIO_WR4_X16_1STOP_NOPARITY
	.db	KOS_SIO_WR_SELECT_WR3, KOS_SIO_WR3_RX8_NO_AUTO_ENABLES
	.db	KOS_SIO_WR_SELECT_WR5, KOS_SIO_WR5_TX8_DTR_RTS_ASSERT
	.db	KOS_SIO_WR_SELECT_WR1, KOS_SIO_WR1_INT_DISABLED
con_init_end:

ioc_init_seq:
	.db	KOS_IOC_WR0_RESET_CHANNEL
	.db	KOS_SIO_WR_SELECT_WR1, KOS_IOC_WR1_INT_WAIT_OFF
	.db	KOS_SIO_WR_SELECT_WR4, KOS_IOC_WR4_EXTSYNC_X1
	.db	KOS_IOC_WR_SELECT_WR7, KOS_IOC_WR7_UNDERRUN_FILL
	.db	KOS_SIO_WR_SELECT_WR3, KOS_IOC_WR3_RX_OFF
	.db	KOS_SIO_WR_SELECT_WR5, KOS_IOC_WR5_TX_RTS_OFF
ioc_init_end:

; SYNC, LEN = 3, TYPE, SEQ, STATUS, CRC-16-CCITT over LEN..STATUS (MSB
; first; tests/z80sim.py ioc_crc16).
reset_frame::
	.db	KOS_IOC_PACKET_SYNC0, KOS_IOC_PACKET_SYNC1
	.db	KOS_IOC_PACKET_FIXED_LEN, 0x00
	.db	IOC_CMD_RESET, 0x01, 0x00
	.db	0xB3, 0x83
reset_frame_end:

txt_banner:	.ascii	"KonaOS Mk2 boot "
		.db	0
txt_sys:	.ascii	"sys:"
		.db	0
txt_wrk:	.ascii	" wrk:"
		.db	0
txt_sys_again:	.ascii	" sys:"
		.db	0
txt_ram_sys:	.ascii	" ram sys:"
		.db	0
txt_ram_wrk:	.ascii	" wrk:"
		.db	0
txt_reboot:	.ascii	"reboot"
		.db	0x0D, 0x0A, 0
txt_reboot_failed:
		.ascii	"reboot: no reset"
		.db	0x0D, 0x0A, 0
txt_done:	.ascii	"done"
		.db	0x0D, 0x0A, 0
payload_end::
