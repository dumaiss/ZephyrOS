; -----------------------------------------------------------------------------
; The IOC-FAT truncate operation (iocfat.c, KFILE_OP_TRUNCATE) and the
; extending-write size note, hand-written because their 32-bit compares and
; copies cost several times as much compiled and the operation must fit the
; driver hot budget.  sdcccall(0): the frame arrives in HL, int16 results
; leave in DE; IX and IY are preserved.
;
; struct iocfat_file (21 bytes, iocfat.c): used 0, flags 1, token 2..3,
; generation 4..7, offset 8..11, size 12..15, key 16, next 17..20.
; iocfat.read_only is byte 54 of the `iocfat` instance.
; -----------------------------------------------------------------------------

	.module iocfat_trunc

	.globl _iocfat_file_get
	.globl _iocfat_file_generation
	.globl _iocfat_exchange
	.globl _iocfat_map_status
	.globl _iocfat_cache_drop_all
	.globl _iocfat
	.globl _iocfat_request
	.globl _iocfat_reply

FILE_FLAGS     = 1
FILE_OFFSET    = 8
FILE_SIZE      = 12
INST_READ_ONLY = 54

FS2_CMD_TRUNCATE = 0x40

; The request frame: data[0..1] the token, data[2..5] the length.
FR_TOKEN      = 3
FR_TRUNC_SIZE = 5

	.area _CODE

; In: HL = A's most significant byte pointer, DE = B's.  Out: C = B <= A
; (both unsigned, the highest differing byte decides; equal sets C
; through SCF).  Flags survive the exit; B, HL and DE are clobbered.
tr_cmp_msb:
	ld	b,#4
tr_cm:
	ld	a,(de)
	cp	(hl)
	ret	nz
	dec	de
	dec	hl
	djnz	tr_cm
	scf				; equal: C set, like the shrink sense
	ret

; In: HL = the destination byte, DE = the source byte, B = the count from
; the differing byte down to the least significant.  Copies B bytes
; downward and leaves DE = 0 (the truncate success value); B, HL and
; DE are clobbered.
tr_copy_down:
	ld	a,(de)
	ld	(hl),a
	dec	hl
	dec	de
	djnz	tr_copy_down
	ld	de,#0x0000
	ret

; ---------------------------------------------------------------------------
; int16_t iocfat_truncate_op(struct kos_ipc_frame *frame);
;   HL = frame.  The length (payload[1..4]) at or past the size is a no-op;
;   below it, the FS2 TRUNCATE, then the whole cache drops, the size takes
;   the length and the position past the new end clamps to it.
; ---------------------------------------------------------------------------
_iocfat_truncate_op::
	push	ix
	push	hl
	pop	ix			; IX = the frame
	ld	a,6 (ix)		; payload[0]: the instance identity
	call	_iocfat_file_get	; DE = the file, or 0
	ld	a,d
	or	a,e
	jp	nz,tr_file
	ld	de,#0xfffe		; -KOS_EINVAL
	jp	tr_ret
tr_file:
	push	de			; the file
	ex	de,hl
	call	_iocfat_file_generation
	ld	a,d
	or	a,e
	jp	z,tr_open
	pop	hl			; drop the file; DE = the error
	jp	tr_ret
tr_open:
	pop	de			; the file
	; A write-open handle on a writable card, as iocfat_file_write.
	inc	de
	ld	a,(de)			; the flags
	dec	de			; the file again
	rrca
	jp	nc,tr_ro
	ld	a,(_iocfat+INST_READ_ONLY)
	or	a,a
	jp	nz,tr_ro
	; fall through: the file in DE, the write flag set, the card
	; writable
tr_go:
	push	de			; the file, kept across the exchange
	; The no-op rule (FatFs truncation only shrinks): the length
	; against the size, most significant byte first.
	push	ix
	pop	hl
	ld	bc,#0x000A
	add	hl,bc			; HL = frame+10 (length MSB)
	ex	de,hl			; DE = length MSB, HL = file
	ld	bc,#0x000F
	add	hl,bc
	ex	de,hl			; DE = size MSB, HL = length MSB
	call	tr_cmp_msb
	jr	c,tr_noop		; size <= length: past the size
tr_shrink:
	pop	de			; the file
	push	de
	; request.data[0..1] = the token, [2..5] = the length bytes.
	ex	de,hl			; HL = file (kept on the stack)
	inc	hl
	inc	hl			; HL = the token
	ld	de,#(_iocfat_request+FR_TOKEN)
	ld	bc,#0x0002
	ldir				; DE = request+5 (FR_TRUNC_SIZE)
	push	ix
	pop	hl
	ld	bc,#0x0007
	add	hl,bc			; payload[1]
	ld	bc,#0x0004
	ldir				; data[2..5] = the length
	; The exchange, then its status.
	ld	l,#0x06
	ld	a,#FS2_CMD_TRUNCATE
	call	_iocfat_exchange
	ld	a,d
	or	a,e
	jr	nz,tr_fail		; the transport error
tr_status:
	ld	a,(_iocfat_reply+1)
	ld	l,#0x00
	call	_iocfat_map_status
	ld	a,d
	or	a,e
	jr	nz,tr_fail
	call	_iocfat_cache_drop_all
	pop	de			; the file
	push	de			; ...and keep it for the clamp
	; size = length, from the request.
	ld	hl,#(_iocfat_request+FR_TRUNC_SIZE)
	ex	de,hl			; DE = request+5, HL = file
	ld	bc,#FILE_SIZE
	add	hl,bc
	ex	de,hl			; DE = size, HL = request+5
	ld	bc,#0x0004
	ldir				; size = length
	; The clamp: the position against the (new) size, both in the
	; file.
	pop	hl			; HL = file
	ld	de,#FILE_OFFSET+3
	add	hl,de			; HL = offset MSB (file+11)
	push	hl			; ...kept across the size add
	ld	de,#FILE_SIZE-FILE_OFFSET
	add	hl,de			; HL = size MSB (file+15)
	ex	de,hl			; DE = size MSB
	pop	hl			; HL = offset MSB
	call	tr_cmp_msb
	jr	nc,tr_zero		; offset < size: it stays
	jr	z,tr_zero		; equal: nothing to copy
	; offset > size: offset = size, from the differing byte down
	; (the walk leaves HL = offset byte, DE = size byte, B = the
	; count); the copy leaves DE = 0 and returns.
	pop	ix			; the callee-saved register
	jp	tr_copy_down
tr_fail:
	pop	hl			; drop the file
	jp	tr_ret			; DE = the error
tr_noop:
	pop	de			; drop the file
tr_zero:
	ld	de,#0x0000
tr_ret:
	pop	ix
	ret
tr_ro:
	ld	de,#0xffec		; -KOS_EROFS
	jp	tr_ret

; ---------------------------------------------------------------------------
; void iocfat_write_note(struct iocfat_file *handle);
;   HL = handle.  An extending write's end becomes the size: the position
;   against the size, most significant byte first, and the position copied
;   over when it is larger (a write below the size never shrinks it).
; ---------------------------------------------------------------------------
_iocfat_write_note::
	ld	de,#FILE_OFFSET+3
	add	hl,de			; HL = offset MSB (handle+11)
	push	hl
	ld	bc,#FILE_SIZE-FILE_OFFSET
	add	hl,bc			; HL = size MSB (handle+15)
	ex	de,hl			; DE = size MSB
	pop	hl			; HL = offset MSB
	call	tr_cmp_msb		; C = size <= offset
	ret	nc			; size < offset: no extension
	ret	z			; equal: nothing to copy
	; size > offset: size = offset (the walk leaves HL = offset
	; byte, DE = size byte, B = the count; the shared copy moves
	; them the other way).
	ex	de,hl			; HL = size byte, DE = offset byte
	call	tr_copy_down
	ret
