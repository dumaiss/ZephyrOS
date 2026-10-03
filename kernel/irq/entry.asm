; M2 common IM2 source stubs and non-nested mode-crossing gate.
	.module irq_entry
	.include "zephyr80/hardware.inc"
	.include "zephyr80/memory.inc"
	.include "zephyr80/irq.inc"

	.globl _irq_dispatch
	.globl _irq_active_source
	.globl irq_common_entry
	.globl irq_ctc0_stub, irq_ctc1_stub, irq_ctc2_stub, irq_ctc3_stub
	.globl irq_sio0_stub, irq_sio1_stub, irq_ff_unexpected
	.globl irq_saved_sp, irq_saved_latch, irq_common_sp
	.globl irq_transition_stack, irq_transition_stack_top
	.globl irq_isr_stack, irq_isr_stack_top
	.globl _irq_arch_lock, _irq_arch_unlock
	.globl _irq_kernel_code_end

	.area _COMMON

; Preserve interrupted AF on the still-visible foreground stack before
; loading a source ID.  irq_common_entry records SP before it pushes.
irq_ctc0_stub::
	push	af
	ld	a,#0
	jp	irq_common_entry
irq_ctc1_stub::
	push	af
	ld	a,#1
	jp	irq_common_entry
irq_ctc2_stub::
	push	af
	ld	a,#2
	jp	irq_common_entry
irq_ctc3_stub::
	push	af
	ld	a,#3
	jp	irq_common_entry
irq_sio0_stub::
	push	af
	ld	a,#4
	jp	irq_common_entry
irq_sio1_stub::
	push	af
	ld	a,#5
	jp	irq_common_entry
irq_ff_unexpected::
	push	af
	ld	a,#6
	jp	irq_common_entry

irq_common_entry::
	ld	(_irq_active_source),a
	ld	(irq_saved_sp),sp
	ld	sp,#irq_transition_stack_top

	push	bc
	push	de
	push	hl
	push	ix
	push	iy
	ex	af,af'
	push	af
	exx
	push	bc
	push	de
	push	hl
	exx

	in	a,(ZOS_MEMCTL_PORT)
	ld	(irq_saved_latch),a
	and	#ZOS_MODE_MASK
	cp	#ZOS_MODE_APPLICATION
	jr	nz,100$
	ld	a,(irq_saved_latch)
	and	#ZOS_BANK_MASK
	or	#ZOS_MODE_OS
	out	(ZOS_MEMCTL_PORT),a
100$:
	ld	(irq_common_sp),sp
	ld	sp,#irq_isr_stack_top
	call	_irq_dispatch
	ld	sp,(irq_common_sp)

	ld	a,(irq_saved_latch)
	out	(ZOS_MEMCTL_PORT),a

	exx
	pop	hl
	pop	de
	pop	bc
	exx
	pop	af
	ex	af,af'
	pop	iy
	pop	ix
	pop	hl
	pop	de
	pop	bc
	ld	sp,(irq_saved_sp)
	pop	af
	reti

; Capture IFF2 through LD A,I, then mask.  SDCC consumes a uint8_t return
; in A; provide the state in both A and L so every caller convention sees
; the same value.  (The pre-M3 comment claiming "result in L" was wrong:
; callers read A, which previously leaked I=FDh into the unlock state.)
_irq_arch_lock::
	ld	a,i
	di
	ld	l,#0
	jp	po,100$
	inc	l
100$:
	ld	a,l
	ret

; sdcccall(1): uint8_t argument in A.  EI takes effect after RET.
_irq_arch_unlock::
	or	a
	ret	z
	ei
	ret

; Fixed transition state and stack; no ordinary allocation may enter here.
	.area _IRQ_COMMON_STATE (ABS)
	.org ZOS_IRQ_STATE_BASE
irq_saved_latch::
	.ds	1
irq_saved_sp::
	.ds	2
irq_common_sp::
	.ds	2
_irq_active_source::
	.ds	1
	.ds	(ZOS_IRQ_STACK_BASE - ZOS_IRQ_STATE_BASE - ZOS_IRQ_STATE_HEADER_SIZE)
irq_transition_stack::
	.ds	ZOS_IRQ_STACK_SIZE
irq_transition_stack_top::

	.area _IRQ_ISR_STACK (ABS)
	.org ZOS_IRQ_ISR_STACK_BASE
irq_isr_stack::
	.ds	ZOS_IRQ_ISR_STACK_SIZE
irq_isr_stack_top::
