; -----------------------------------------------------------------------------
; ZephyrOS M10 spike: far-segment syscall gate.
;
; Compiled into an auxiliary segment (no libzos): converts the private
; SDCC register convention to the native RST 30h ABI and maps a carry-set
; error to -errno, all inline.  The argument block the caller passes must
; live in the auxiliary low window (a stack local), because a syscall
; issued from far code sees only 0000h-1FFFh of the execution bank.
;
;   int seg_rst(struct seg_req *req);   HL = req
;   struct seg_req { unsigned char number; void *block; };
; -----------------------------------------------------------------------------

	.module segcall

	.globl _seg_rst

	.area _CODE

_seg_rst::
	ld	a,(hl)			; syscall number
	inc	hl
	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	ex	de,hl			; HL = argument block
	rst	0x30
	jr	nc,001$
	; carry set: A = errno -> DE = -errno
	ld	e,a
	ld	d,#0
	xor	a
	sub	e
	ld	e,a
	ld	a,#0
	sbc	a,d
	ld	d,a
	ret
001$:
	ex	de,hl			; primary result -> DE
	ret
