; KonaOS IM2 page initialization and interrupt-mode selection.
; Kernel-owned IRQ executive initialization; the platform declares the
; vector-page geometry and the I-register value in irq.inc.
	.module platform_irq
	.include "irq.inc"
	.globl _irq_arch_init, irq_ctc0_stub, irq_ff_unexpected

	.area _COMMON
_irq_arch_init::
	di
	; Seed the vector page with deterministic alternating target bytes.
	; Only explicitly installed vectors are guaranteed entry points.
	ld	hl,#irq_ff_unexpected
	ld	a,l
	ld	c,h
	ld	hl,#KOS_IRQ_VECTOR_BASE
	ld	b,#0x80
100$:
	ld	(hl),a
	inc	hl
	ld	(hl),c
	inc	hl
	djnz	100$
	; CTC0 source: vector base 00h -> vector byte 00h.
	ld	hl,#irq_ctc0_stub
	ld	(KOS_IRQ_VECTOR_BASE),hl
	; The final fill store overlaps the linked FF vector word at FDFF/FE00.
	; Restore that mandatory fallback from its linked target address.
	ld	hl,#irq_ff_unexpected
	ld	(KOS_IRQ_VECTOR_BASE + 0x00FF),hl
	; Kernel-owned I register and interrupt mode; EI only after every
	; enabled entry and all transition state are valid.
	ld	a,#KOS_IM2_I_VALUE
	ld	i,a
	im	2
	ei
	ret
