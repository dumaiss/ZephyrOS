; -----------------------------------------------------------------------------
; Zephyr-80 IRQ/vector mapping constants (assembly visible).
;
; IM2 vector-page geometry, the common IRQ transition state/stack
; reservations, the kernel-owned I-register value, and the CTC0 vector
; mapping.  Keep value-identical with irq.h (parity-tested).
;
; Hardware port numbers and CTC/SIO register constants stay in hardware.inc.
; General memory-region geometry stays in memory.inc.
; -----------------------------------------------------------------------------

; IM2 vector page: 256-byte-aligned FD00h page plus the vector-FFh guard
; byte at FE00h (the FFh vector reads one byte past the nominal page).
ZOS_IRQ_VECTOR_BASE     = 0xFD00
ZOS_IRQ_VECTOR_SIZE     = 0x0101

; Common IRQ transition state: a fixed header followed by the saved
; interrupted state, starting at FE01h.
ZOS_IRQ_STATE_BASE      = 0xFE01
ZOS_IRQ_STATE_HEADER_SIZE = 0x0006

; 128-byte common IRQ transition stack, above the state block.
ZOS_IRQ_STACK_BASE      = 0xFE80
ZOS_IRQ_STACK_TOP       = 0xFF00
ZOS_IRQ_STACK_SIZE      = 0x0080

; Kernel-owned IM2 vector page base (the value loaded into the I register).
ZOS_IM2_I_VALUE         = 0xFD

; CTC0 (physical channel 0, port 40h) IM2 vector byte.
ZOS_CTC0_VECTOR_BASE    = 0x00
