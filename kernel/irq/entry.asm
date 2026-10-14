; Common IM2 source stubs and the non-nested mode-crossing, P-normalizing
; gate (revision 13).
	.module irq_entry
	.include "hardware.inc"
	.include "memory.inc"
	.include "irq.inc"

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

	in	a,(KOS_MEMCTL_PORT)
	ld	(irq_saved_latch),a
	; Revision 13: the IRQ gate normalizes the page field.  The whole
	; latch (Q, mode, P) is saved in the gate's own slot above; dispatch
	; then runs with P = 0, the interrupted bank and OS mode, so
	; nucleus-owned handlers -- including drivers still linked into the
	; arena in the cold half -- see the fixed core and the whole bank-7
	; decode.  The exit path below restores the saved latch exactly, so
	; an interrupted window call (4.5) finds its page selection
	; untouched.
	and	#(KOS_MODE_MASK | KOS_BANK_MASK)
	or	#KOS_MODE_OS
	out	(KOS_MEMCTL_PORT),a
	ld	(irq_common_sp),sp
	ld	sp,#irq_isr_stack_top
	call	_irq_dispatch
	ld	sp,(irq_common_sp)

	ld	a,(irq_saved_latch)
	out	(KOS_MEMCTL_PORT),a

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
	; Accepting the interrupt cleared IFF1 and IFF2, and RETI only copies
	; IFF2 back into IFF1.  EI here restores maskable interrupts (it takes
	; effect after RETI, so the exit is still one delivery deep).
	ei
	reti

; Capture IFF2 through LD A,I, then mask.  SDCC consumes a uint8_t return
; in A; provide the state in both A and L so every caller convention sees
; the same value: returning it only in L would leak I = FDh (left in A by
; LD A,I) into the unlock state of callers that read A.
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
; The state cells sit directly below the stack base and the region above
; the stack top is the common boot stack: the gate's saves can only ever
; reach the state cells, never the frames of the interrupted chain.
	.area _IRQ_COMMON_STATE (ABS)
	.org KOS_IRQ_STATE_BASE
irq_saved_latch::
	.ds	1
irq_saved_sp::
	.ds	2
irq_common_sp::
	.ds	2
_irq_active_source::
	.ds	1
	.ds	(KOS_IRQ_STACK_BASE - KOS_IRQ_STATE_BASE - KOS_IRQ_STATE_HEADER_SIZE)
irq_transition_stack::
	.ds	KOS_IRQ_STACK_SIZE
irq_transition_stack_top::

	.area _IRQ_ISR_STACK (ABS)
	.org KOS_IRQ_ISR_STACK_BASE
irq_isr_stack::
	.ds	KOS_IRQ_ISR_STACK_SIZE
irq_isr_stack_top::
