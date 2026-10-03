; Linked FFh-vector guard identity.  The full page is initialized by
; irq_arch_init before IM2 is enabled; this word makes the final image's
; FDFFh/FE00h relationship independently checkable.
	.module irq_vectors
	.include "zephyr80/irq.inc"
	.globl irq_vector_page, irq_vector_guard, irq_ff_unexpected

	.area _IRQ_VECTOR (ABS)
	.org ZOS_IRQ_VECTOR_BASE
irq_vector_page::
	.ds	0x00FF
	.dw	irq_ff_unexpected
irq_vector_guard = ZOS_IRQ_VECTOR_BASE + 0x0100
