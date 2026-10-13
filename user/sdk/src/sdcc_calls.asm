; -----------------------------------------------------------------------------
; KonaOS libkos: SDCC z80 runtime helpers for indirect calls.
;
; The SDK is freestanding (no libsdcc is linked), so the trampolines the
; z80 code generator emits for an indirect call - `farcall`'s plain
; CALL through the far entry pointer is one - must be provided here, as
; the kernel (kernel/runtime/sdcc_helpers.asm) and every module
; (drivers/runtime/sdcc_indirect_calls.asm) provide their own copy.  All
; three helpers are position independent, so any program that references
; one pulls this single archive member; a program that makes no indirect
; call never links it.
; -----------------------------------------------------------------------------

	.module kos_sdcc_calls
	.globl ___sdcc_call_iy, ___sdcc_call_hl, ___sdcc_enter_ix

	.area _CODE

; int __sdcc_call_iy(int); indirect call through IY.
___sdcc_call_iy::
	jp	(iy)

; int __sdcc_call_hl(int); indirect call through HL.
___sdcc_call_hl::
	jp	(hl)

; The --opt-code-size frame entry: the caller CALLs this helper, which
; pops the return address, saves IX, points IX at the new frame and
; resumes the function body (canonical sdcc z80 crtenter.s helper, see
; the kernel copy).
___sdcc_enter_ix::
	pop	hl
	push	ix
	ld	ix, #0
	add	ix, sp
	jp	(hl)
