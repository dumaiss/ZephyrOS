; -----------------------------------------------------------------------------
; ZephyrOS mode-11 window copy (kernel/memory, work package 43).
;
; The object-side copy for extended-memory objects in auxiliary slots 3-6
; (architecture 36.16): the transfer runs with the memory mode and bank
; field preserved and the page field set to 7 - bank, so the window
; 6000h-DFFFh shows that auxiliary bank at the object's own addresses.
; Everything else the primitive touches stays mapped: 2000h-5FFFh shows
; bank 7's fixed core (this code), E000h-FFFFh shows bank 0 (the common
; staging buffer and the caller's stack).  One LDIR moves the run; no
; stack move and no interrupt-push reservation are needed, because the
; caller's stack is common memory, visible whatever the window shows.
;
; Slots 0-2 live at 0000h-5FFFh, which the window cannot show, so they use
; the mode-10 application-bank copy gate (kernel/memory/user_copy.asm)
; instead.
;
; Fixed rule (architecture 36.5): code that executes while the window
; shows another bank lives in the fixed core, so this file links into
; _CODE at 2000h-5FFFh.  Common memory is the scarcest region and does not
; pay for it.
;
; Interface (kernel-internal, not a native ABI).  The request has the same
; layout as `struct zos_bank_copy_req` (include/zephyros/process.h); it is
; read in mode 11 with the caller's latch still in force, so it may live on
; the caller's stack.  `bank` is the auxiliary bank (0-5; bank 6 would
; select the operating system's own service window), `addr` the object
; address in the window and `buffer` the common staging address:
;
;   uint8_t zos_window_copy(const struct zos_bank_copy_req *req);
;
; SDCC sdcccall(1): a single 16-bit pointer argument arrives in HL; the
; 8-bit result is returned in A (0, or ZOS_EINVAL).
;
; Single-depth: not reentrant (single-tasking, like the other copy gates).
; -----------------------------------------------------------------------------

	.module window_copy
	.include "zephyr80/hardware.inc"
	.include "zephyr80/memory.inc"

	.globl _zos_window_copy
	.globl zos_window_copy_end
	.globl zos_window_bank
	.globl zos_window_op
	.globl zos_window_addr
	.globl zos_window_buffer
	.globl zos_window_length
	.globl zos_window_saved_latch
	.globl zos_window_saved_iff

	.area _CODE

; Operation codes (mirror zephyros/process.h).
ZOS_COPY_OP_READ  = 1
ZOS_COPY_OP_WRITE = 2

; ---------------------------------------------------------------------------
; _zos_window_copy - C name zos_window_copy.  Entry: HL = request pointer.
;
; Decode and validate everything while bank 7 is still fully visible (the
; caller runs with P = 0), then switch only the page field and run the
; LDIR.  Clobbers: AF, BC, DE, HL.
; ---------------------------------------------------------------------------
_zos_window_copy::
	ld	a,(hl)			; bank
	cp	#6			; 0-5 only
	jr	c,001$
	ld	a,#0x02			; ZOS_EINVAL
	ret
001$:
	ld	(zos_window_bank),a
	inc	hl
	ld	a,(hl)			; op
	ld	(zos_window_op),a
	dec	a			; valid ops are 1..2 -> 0..1
	cp	#0x02
	jr	c,002$
	ld	a,#0x02
	ret
002$:
	inc	hl
	ld	a,(hl)			; addr (object address in the window)
	ld	(zos_window_addr),a
	inc	hl
	ld	a,(hl)
	ld	(zos_window_addr+1),a
	inc	hl
	ld	a,(hl)			; buffer (common staging)
	ld	(zos_window_buffer),a
	inc	hl
	ld	a,(hl)
	ld	(zos_window_buffer+1),a
	inc	hl
	ld	a,(hl)			; length
	ld	(zos_window_length),a
	inc	hl
	ld	a,(hl)
	ld	(zos_window_length+1),a

	; A zero length is a silent success (bank/op still validated).
	ld	a,(zos_window_length)
	or	a
	jr	nz,003$
	ld	a,(zos_window_length+1)
	or	a
	jr	nz,003$
	xor	a
	ret

003$:
	ld	a,i			; snapshot IFF2 before DI clears it
	di
	ld	a,#0
	jp	po,004$
	inc	a
004$:
	ld	(zos_window_saved_iff),a
	in	a,(ZOS_MEMCTL_PORT)
	ld	(zos_window_saved_latch),a
	and	#(ZOS_MODE_MASK | ZOS_BANK_MASK)
	ld	c,a
	ld	a,(zos_window_bank)
	ld	b,a
	ld	a,#7
	sub	b			; page = 7 - bank (1..7)
	rrca
	rrca
	rrca
	and	#ZOS_ROM_PAGE_MASK
	or	c
	out	(ZOS_MEMCTL_PORT),a
	; Everything below is fetched from the fixed core or common memory;
	; the window now shows the auxiliary bank.
	ld	a,(zos_window_op)
	ld	hl,(zos_window_addr)
	ld	de,(zos_window_buffer)
	ld	bc,(zos_window_length)
	cp	#ZOS_COPY_OP_WRITE
	jr	z,005$
	; READ: window -> buffer (HL = addr, DE = buffer, BC = length)
	ldir
	jr	006$
005$:
	; WRITE: buffer -> window (swap to HL = buffer, DE = addr)
	ex	de,hl
	ldir
006$:
	ld	a,(zos_window_saved_latch)
	out	(ZOS_MEMCTL_PORT),a
	ld	a,(zos_window_saved_iff)
	or	a
	jr	z,007$
	ei
007$:
	xor	a
	ret
zos_window_copy_end::

	.area _DATA

; Single-depth request and transition state (not reentrant; see the module
; header).  Fixed-core `_DATA`: it is read while the window shows the
; auxiliary bank, and 2000h-5FFFh always shows bank 7.
zos_window_bank::
	.ds	1
zos_window_op::
	.ds	1
zos_window_addr::
	.ds	2
zos_window_buffer::
	.ds	2
zos_window_length::
	.ds	2
zos_window_saved_latch::
	.ds	1
zos_window_saved_iff::
	.ds	1
