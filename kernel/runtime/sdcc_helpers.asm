; -----------------------------------------------------------------------------
; Minimal SDCC runtime helpers for the freestanding kernel.
;
; ZephyrOS is freestanding (no libsdcc is linked).  The canonical
; ___sdcc_call_iy trampoline already lives at the end of _CODE in
; kernel/irq/code_end.asm (M2 needs it for the IRQ handler table); this
; file adds the matching ___sdcc_call_hl variant in case the code
; generator selects it for a call pattern, plus the ___sdcc_enter_ix
; frame-setup helper the z80 code generator factors out when compiling
; for size (--opt-code-size) for functions reached through a stack
; argument frame.  All three are one-instruction or fixed sequence
; canonical implementations from the SDCC z80 runtime.
; -----------------------------------------------------------------------------

	.module sdcc_helpers
	.globl ___sdcc_call_hl, ___sdcc_enter_ix

	.area _CODE

; int __sdcc_call_hl(int); indirect call through HL.
___sdcc_call_hl::
	jp	(hl)

; Stack-frame entry used by --opt-code-size: the caller CALLs this
; helper, which pops the return address, saves IX, points IX at the new
; frame and resumes the function body.  Byte-for-byte the sdcc z80
; crtenter.s implementation.
___sdcc_enter_ix::
	pop	hl
	push	ix
	ld	ix, #0
	add	ix, sp
	jp	(hl)
