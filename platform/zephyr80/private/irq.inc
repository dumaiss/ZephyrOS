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
KOS_IRQ_VECTOR_BASE     = 0xFD00
KOS_IRQ_VECTOR_SIZE     = 0x0101

; Common IRQ transition state: a fixed header followed by the saved
; interrupted state, starting at FE01h.
KOS_IRQ_STATE_BASE      = 0xFE01
KOS_IRQ_STATE_HEADER_SIZE = 0x0006

; 128-byte common IRQ transition stack, directly above the state block so
; that an overrun of the reservation can only reach the state cells.  The
; region above its top belongs to the common boot stack (memory.inc): a
; nucleus chain may hold live frames below FF00h, and the gate must save
; into the reservation, not into those frames.
KOS_IRQ_STACK_BASE      = 0xFE08
KOS_IRQ_STACK_TOP       = 0xFE88
KOS_IRQ_STACK_SIZE      = 0x0080

; Kernel-owned IM2 vector page base (the value loaded into the I register).
KOS_IM2_I_VALUE         = 0xFD
