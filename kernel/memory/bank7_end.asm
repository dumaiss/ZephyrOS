; -----------------------------------------------------------------------------
; Cold-half code bound.
;
; The two-pass build measures, in a probe link, the exclusive end of the
; linked cold code area (_WINDOW); tools/mapcheck.py `geometry derive`
; writes it into the generated include `bank7_end.inc`, and the cold
; runtime data base is align_up of the same value (no reservation sits
; between cold code and cold data).
;
; The generated file lives under the build directory and is included via
; the per-build include path; platform/zephyr80/private/memory.inc stays
; declarative.  The probe link sets ZOS_KERNEL_CODE_END = 0, so kernel
; growth can never collide with a placeholder.
;
; Exported symbol (the declaration area contains no bytes; a zero-size ABS
; label only):
;   __kernel_code_end     exclusive end of the linked cold code area
;
; SDCC prefixes C identifiers with one underscore, so C code declaring
; `extern uint8_t __kernel_code_end[]` resolves to the alias symbol
; ___kernel_code_end; both names sit at the same address.
; -----------------------------------------------------------------------------

	.include "bank7_end.inc"

	.module bank7_end
	.globl __kernel_code_end, ___kernel_code_end

	.area _BANK7_END_DECL (ABS)

	.org ZOS_KERNEL_CODE_END
__kernel_code_end::
___kernel_code_end::
