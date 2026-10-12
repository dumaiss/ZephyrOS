; -----------------------------------------------------------------------------
; Module-local SDCC runtime glue, shared by every ZMOD that needs it.
;
; A ZMOD is linked at base zero and may not import kernel symbols, so the
; freestanding SDCC runtime helpers the z80 code generator emits
; (___sdcc_call_iy / ___sdcc_call_hl, plus the ___sdcc_enter_ix frame
; helper selected by --opt-code-size) must be provided inside each module
; image.  The kernel's own copy (kernel/runtime/sdcc_helpers.asm) is
; deliberately NOT referenced: importing it would be a forbidden external
; reference, while these intra-module calls are made position-independent
; by ZREL_ABS16 relocation once the loader has chosen an arena address.
;
; This one source is compiled to a relocatable object and packed into every
; ZMOD that needs it (the SIO module variants and the synthetic test
; fixtures); each packed container carries its own copy.
; -----------------------------------------------------------------------------

	.module sdcc_indirect_calls
	.globl ___sdcc_call_iy, ___sdcc_call_hl, ___sdcc_enter_ix

	.area _CODE

___sdcc_call_iy::
	jp	(iy)

___sdcc_call_hl::
	jp	(hl)

; Canonical sdcc z80 crtenter.s frame-entry helper (see kernel copy).
___sdcc_enter_ix::
	pop	hl
	push	ix
	ld	ix, #0
	add	ix, sp
	jp	(hl)
