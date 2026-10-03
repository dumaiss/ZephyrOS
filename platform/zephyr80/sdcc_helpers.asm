; -----------------------------------------------------------------------------
; Minimal SDCC runtime helper for indirect calls through HL.
;
; ZephyrOS is freestanding (no libsdcc is linked).  The canonical
; ___sdcc_call_iy trampoline already lives at the end of _CODE in
; kernel/irq/code_end.asm (M2 needs it for the IRQ handler table); this
; file adds the matching ___sdcc_call_hl variant in case the code
; generator selects it for a call pattern.  Both are one-instruction
; canonical implementations.
; -----------------------------------------------------------------------------

	.module sdcc_helpers
	.globl ___sdcc_call_hl

	.area _CODE

; int __sdcc_call_hl(int); indirect call through HL.
___sdcc_call_hl::
	jp	(hl)
