; Must be the final object contributing to _CODE.  irq_claim uses this
; linked boundary when checking that M2 handlers reside in bank-7 code.
	.module irq_code_end
	.globl _irq_kernel_code_end, ___sdcc_call_iy
	.area _CODE

; Minimal freestanding SDCC indirect-call trampoline.  The compiler places
; a function pointer in IY and CALLs this symbol; the handler's RET returns
; directly to the compiler-generated call site.
___sdcc_call_iy::
	jp	(iy)

_irq_kernel_code_end::
