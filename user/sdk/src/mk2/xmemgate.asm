; -----------------------------------------------------------------------------
; KonaOS libkos: the far-window helpers, Zephyr-80 Mk2.
;
; Opening the far window (8000h-BFFFh) is not a syscall: the program hands
; the mapping token SYS_XMEM_MAP or SYS_SEG_MAP returned to the far-window
; vector every program page zero carries (RST 28h, docs/abi.md section
; 8.3), which maps the window and returns the token in force before.  The
; token is logical - it names no page - and 0 is the program's own memory,
; so passing back what an open returned closes it.  These two entries are
; the register-only primitives; everything else about the window is
; ordinary code and ordinary pointer accesses (architecture 7.2).
;
; The SDK's mapping operations, Zephyr-80 Mk2 implementation (the Mk1
; members write the banking latch instead).  With vcall.asm beside it this
; is the only SDK source that opens the window; every other SDK mapping
; change calls these entries.  The vector clobbers AF, BC, DE and HL;
; both entries keep DE and HL, as the Mk1 members do: the banked-call
; trampoline (bcall.asm) holds the far entry in HL across the open and
; the far function's result in DE and HL across the close.
; -----------------------------------------------------------------------------

	.module kos_xmemgate

	.globl _aperture_open
	.globl _aperture_close

	.area _CODE

; unsigned char aperture_open(unsigned char token);  A = token.
; Opens the window onto the token and returns the previous token in A.
_aperture_open::
	push	de
	push	hl
	ld	l,a
	ld	h,#0
	rst	0x28			; the far-window vector: HL -> previous HL
	ld	a,l
	pop	hl
	pop	de
	ret

; void aperture_close(unsigned char previous);  A = the token to restore.
_aperture_close::
	push	de
	push	hl
	ld	l,a
	ld	h,#0
	rst	0x28
	pop	hl
	pop	de
	ret
