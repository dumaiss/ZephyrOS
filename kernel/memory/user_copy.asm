; -----------------------------------------------------------------------------
; KonaOS common-resident application-bank copy gate.
;
; The syscall dispatcher and the EXE loader run in mode 11, where only the
; caller window 0000h-1FFFh of the application bank is visible.  This gate
; performs the one bounded transfer that needs the full application
; mapping: it switches to mode 10 over the requested bank in common
; memory, moves bytes through a common-resident buffer, and restores the
; exact latch, stack, and interrupt state.
;
; Interface (kernel-internal, not a native ABI).  The request is read in
; mode 11 - before any state changes - so the request itself may live in
; bank-7 or common kernel memory; `buffer` must be common-resident:
;
;   struct kos_bank_copy_req {
;       uint8_t  bank;   0-6 (bank 7 rejected with KOS_EINVAL)
;       uint8_t  op;     KOS_BANK_OP_READ / _WRITE / _ZERO
;       uint16_t addr;   user-side logical address
;       uint8_t *buffer; common staging address (op READ/WRITE)
;       uint16_t length; 0 is a validated no-op
;   };
;   uint8_t kos_bank_copy(const struct kos_bank_copy_req *req);
;       returns 0 or KOS_EINVAL (see kos/errno.h)
;
; SDCC sdcccall(1): a single 16-bit pointer argument arrives in HL; the
; 8-bit result is returned in A.
;
; Single-depth: not reentrant (single-tasking).  The transfer runs with
; SP on the common copy stack, so no banked stack or code is ever touched
; while mode 10 is active.
; -----------------------------------------------------------------------------

	.module user_copy
	.include "hardware.inc"
	.include "memory.inc"

	.globl _kos_bank_copy
	.globl _kos_user_copy_staging
	.globl kos_copy_op
	.globl kos_copy_bank
	.globl kos_copy_addr
	.globl kos_copy_buffer
	.globl kos_copy_length
	.globl kos_copy_saved_sp
	.globl kos_copy_saved_latch
	.globl kos_copy_saved_iff
	.globl kos_copy_stack
	.globl kos_copy_stack_top

	.area _COMMON

; Operation codes (mirror process.h).
KOS_COPY_OP_READ  = 1
KOS_COPY_OP_WRITE = 2
KOS_COPY_OP_ZERO  = 3

	.area _COMMON

; ---------------------------------------------------------------------------
; _kos_bank_copy - C name kos_bank_copy.  Entry: HL = request pointer.
;
; Decode and validate everything while the request is still visible, then
; switch mapping and perform the transfer from common state.
; ---------------------------------------------------------------------------
_kos_bank_copy::
	ld	a,(hl)			; bank
	cp	#KOS_BANK_MASK		; bank 7 and above are not application
	jr	c,001$
	ld	a,#0x02			; KOS_EINVAL
	ret
001$:
	ld	(kos_copy_bank),a
	inc	hl
	ld	a,(hl)			; op
	ld	(kos_copy_op),a
	dec	a			; valid ops are 1..3 -> 0..2
	cp	#0x03
	jr	c,002$
	ld	a,#0x02
	ret
002$:
	inc	hl
	ld	a,(hl)			; addr
	ld	(kos_copy_addr),a
	inc	hl
	ld	a,(hl)
	ld	(kos_copy_addr+1),a
	inc	hl
	ld	a,(hl)			; buffer
	ld	(kos_copy_buffer),a
	inc	hl
	ld	a,(hl)
	ld	(kos_copy_buffer+1),a
	inc	hl
	ld	a,(hl)			; length
	ld	(kos_copy_length),a
	inc	hl
	ld	a,(hl)
	ld	(kos_copy_length+1),a

	; A zero length is a silent success (bank/op still validated).
	ld	a,(kos_copy_length)
	or	a
	jr	nz,003$
	ld	a,(kos_copy_length+1)
	or	a
	jr	nz,003$
	xor	a
	ret

003$:
	ld	a,i			; snapshot IFF2 before DI clears it
	di
	ld	(kos_copy_saved_sp),sp
	ld	sp,#kos_copy_stack_top
	ld	a,#0
	jp	po,004$
	inc	a
004$:
	ld	(kos_copy_saved_iff),a
	in	a,(KOS_MEMCTL_PORT)
	ld	(kos_copy_saved_latch),a
	ld	a,(kos_copy_bank)
	or	#KOS_MODE_APPLICATION
	out	(KOS_MEMCTL_PORT),a
	; Everything below is fetched from common memory; both the
	; application bank and the common data area are now mapped.
	ld	a,(kos_copy_op)
	cp	#KOS_COPY_OP_ZERO
	jr	z,006$
	ld	bc,(kos_copy_length)
	ld	de,(kos_copy_buffer)
	ld	hl,(kos_copy_addr)
	cp	#KOS_COPY_OP_WRITE
	jr	z,005$
	; READ: user -> buffer (HL = user, DE = buffer, BC = length)
	ldir
	jr	007$
005$:
	; WRITE: buffer -> user (swap to HL = buffer, DE = user)
	ex	de,hl
	ldir
	jr	007$
006$:
	; ZERO: fill the user range with zero bytes.
	ld	hl,(kos_copy_addr)
	ld	bc,(kos_copy_length)
	ld	d,h
	ld	e,l
	inc	de
	ld	(hl),#0
	dec	bc
	ld	a,b
	or	c
	jr	z,007$
	ldir
007$:
	ld	a,(kos_copy_saved_latch)
	out	(KOS_MEMCTL_PORT),a
	ld	sp,(kos_copy_saved_sp)
	ld	a,(kos_copy_saved_iff)
	or	a
	jr	z,008$
	ei
008$:
	xor	a
	ret

	.area _COMMON_DATA

; Bounded staging buffer owned by the copy contract: syscall buffers are
; transferred through this common block, never through a banked pointer.
; 64 bytes keeps the gate cheap while the syscall driver owns the
; chunking.  (C name kos_user_copy_staging.)
_kos_user_copy_staging::
	.ds	64

; Decoded request and transition state (single-depth; see the module
; header).  The op byte doubles as the end-of-staging marker for the
; mapcheck size proof.
kos_copy_op::
	.db	KOS_COPY_OP_READ
kos_copy_bank::
	.ds	1
kos_copy_addr::
	.ds	2
kos_copy_buffer::
	.ds	2
kos_copy_length::
	.ds	2
kos_copy_saved_sp::
	.ds	2
kos_copy_saved_latch::
	.ds	1
kos_copy_saved_iff::
	.ds	1
kos_copy_stack::
	.ds	16
kos_copy_stack_top::
