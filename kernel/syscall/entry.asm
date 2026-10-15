; -----------------------------------------------------------------------------
; KonaOS native syscall gate and dispatch boundary.
;
; Native entry is RST 30h; page zero at 0030h holds JP kos_syscall_entry
; (see page_zero.asm).  Everything on the crossing path is in permanent
; common memory; the dispatcher boundary and the dispatcher itself are in
; bank 7.
;
; Entry state (documented ABI, docs/abi.md):
;   mode 10 active
;   A  = syscall number
;   HL = pointer to the argument block (must lie in 0000h-1FFFh)
;   SP = caller stack, possibly in 2000h-DFFFh (hidden by mode 11)
;
; The caller's interrupt state is snapshotted at entry and restored for the
; body (the crossing's own steps stay masked), then restored to the caller
; on return; see the comments at both points.
;
; Return state:
;   carry set, A = error code, HL = primary result (carry clear: success)
;   BC, DE, IX, IY preserved; SP and the selected bank restored;
;   mode 10 restored; other flags unspecified
;
; Single-entry and non-reentrant; the common transition state is
; single-depth.  SYS_EXIT never takes this return path: the C dispatcher
; tears the process down and resumes the parent through the process gate.
;
; Register clobbers are documented per routine.  Nothing here exposes SDCC
; calling conventions to syscall callers.
; -----------------------------------------------------------------------------

	.module syscall_entry
	.include "hardware.inc"
	.include "memory.inc"

	.globl kos_syscall_entry
	.globl kos_syscall_dispatch
	.globl __kos_mode_fault
	.globl kos_gate_saved_sp
	.globl kos_gate_saved_latch
	.globl kos_gate_result_hl
	.globl kos_gate_result_a
	.globl kos_gate_result_c
	.globl kos_gate_saved_iff
	.globl kos_gate_saved_a
	.globl kos_gate_stack_top
	.globl _kernel_syscall_dispatch
	.globl _kos_event_syscall_exit
	.globl _kos_syscall_number
	.globl _kos_syscall_args
	.globl _kos_syscall_result_a
	.globl _kos_syscall_result_c
	.globl _kos_syscall_result_hl

	.area _COMMON

; ---------------------------------------------------------------------------
; kos_syscall_entry - common-memory syscall gate.
;
; An RST has already pushed the return PC on the caller's stack, which may
; be anywhere in the selected application bank and may disappear above
; 1FFFh once mode 11 is entered.  The gate therefore never pushes before
; switching stacks.
;
; Clobbers: none observable (see ABI); internally AF/HL/C/SP.
; ---------------------------------------------------------------------------
kos_syscall_entry::
	; Save A before LD A,I, then atomically snapshot IFF2 and mask IRQs.
	; An IRQ between the first store and DI cannot observe transition state:
	; no mapping or stack state has changed yet, and IRQ state is disjoint.
	ld	(kos_gate_saved_a),a
	; IFF2 is read up to three times: on the board an interrupt
	; accepted right after LD A,I makes that read report 0 (the
	; documented LD A,I behaviour), and the interrupt's own EI/RETI
	; makes the next read true.
	ld	a,i
	jp	pe,entry_iff1
	ld	a,i
	jp	pe,entry_iff1
	ld	a,i
entry_iff1:
	di
	ld	a,#0
	jp	po,001$
	inc	a
001$:
	ld	(kos_gate_saved_iff),a

	; ED 73 captures SP without consuming HL or touching the caller stack.
	ld	(kos_gate_saved_sp),sp

	; Switch to the reserved common gate stack BEFORE the mapping changes,
	; then save the ABI-preserved registers before any of them is used as
	; scratch.
	ld	sp,#kos_gate_stack_top
	push	bc
	push	de
	push	ix
	push	iy
	push	hl			; argument-block pointer for the dispatcher
	ld	a,(kos_gate_saved_a)
	push	af			; syscall number for the dispatcher

	; Validate the entry state: mode 10 over an application bank 0-6.
	; Anything else takes the defined common-resident fatal path rather
	; than returning through an unsafe mapping.
	in	a,(KOS_MEMCTL_PORT)
	ld	(kos_gate_saved_latch),a	; preserve the full caller latch
	and	#KOS_MODE_MASK
	cp	#KOS_MODE_APPLICATION
	jp	nz,__kos_mode_fault
	ld	a,(kos_gate_saved_latch)
	and	#KOS_BANK_MASK
	cp	#KOS_BANK_MASK		; bank 7 is not an application bank
	jp	z,__kos_mode_fault

	; Enter OS mode over the caller bank: bank and mode only, so the
	; revision-13 page field is cleared and the whole bank-7 decode is
	; visible (architecture 4.2: the syscall gate is P-normalizing).
	ld	a,(kos_gate_saved_latch)
	and	#KOS_BANK_MASK
	or	#KOS_MODE_OS
	out	(KOS_MEMCTL_PORT),a

	; The body runs under the caller's interrupt state: the CTC holds one
	; interrupt, so a body that ran masked for longer than two ticks would
	; lose timer ticks (and a benchmark would measure its own loss).  Every
	; step above stays masked - it writes the common gate state, switches
	; SP and changes the mapping - and the return path masks again before
	; it touches any of it.  Delivery is safe here: the IRQ executive keeps
	; its own state and stacks, and restores the exact latch.
	ld	a,(kos_gate_saved_iff)
	or	a
	jr	z,003$
	ei
003$:

	; Restore the syscall inputs; bank 7 is visible at 2000h-DFFFh.
	pop	af
	pop	hl

	; Dispatch.  Boundary contract (internal): A = syscall number,
	; HL = arg block; returns HL = primary result, A = error code,
	; carry set.
	call	kos_syscall_dispatch

	; Mask again before touching the gate's saved state: the body ran with
	; the caller's interrupt state, so this is where the crossing takes
	; interrupts away again (the caller gets them back at 002$).
	di

	; Stash the results in common state so the return path can rebuild
	; registers exactly.
	ld	(kos_gate_result_hl),hl
	ld	(kos_gate_result_a),a
	ld	a,#0x00
	rla			; carry -> bit 0 of A (rla preserves carry)
	ld	(kos_gate_result_c),a

	; Restore the preserved registers.
	pop	iy
	pop	ix
	pop	de
	pop	bc

	; Back to application mode over the caller bank (common code; the
	; next instructions are fetched from memory mapped in both modes).
	ld	a,(kos_gate_saved_latch)
	out	(KOS_MEMCTL_PORT),a

	; Restore the exact caller SP; the caller stack is visible again.
	ld	hl,(kos_gate_saved_sp)
	ld	sp,hl

	; Device events (architecture 4.10): the caller is about to become the
	; running code again, so a pending event of its own is delivered now,
	; before the result registers are rebuilt.  The check either returns
	; here - nothing deliverable - or never returns: it builds the frame
	; below the caller's RST return address, puts the syscall's result in
	; it and hands off through RETI to the libkos trampoline.
	call	_kos_event_syscall_exit

	; Restore the prior interrupt state immediately before returning.  The
	; result setup is duplicated so testing saved IFF cannot alter carry.
	ld	a,(kos_gate_saved_iff)
	or	a
	jr	z,002$
	ld	a,(kos_gate_result_c)
	rra
	ld	a,(kos_gate_result_a)
	ld	hl,(kos_gate_result_hl)
	ei
	ret
002$:
	ld	a,(kos_gate_result_c)
	rra
	ld	a,(kos_gate_result_a)
	ld	hl,(kos_gate_result_hl)
	ret

	.area _CODE

; ---------------------------------------------------------------------------
; kos_syscall_dispatch - bank-7 boundary that converts the stable register
; ABI to the internal (private) C calling convention.
;
; In:  A = syscall number, HL = argument block pointer.
; Out: HL = primary result, A = error code; carry set on failure and
;      clear on success (the C dispatcher publishes it in
;      _kos_syscall_result_c).
; Clobbers: AF, BC, DE, HL (internal; the gate restores the ABI state).
; ---------------------------------------------------------------------------
kos_syscall_dispatch::
	ld	(_kos_syscall_number),a
	ld	(_kos_syscall_args),hl
	call	_kernel_syscall_dispatch
	; Rebuild the result: carry = bit 0 of the C result flag.  RRA moves
	; the flag bit into carry; the subsequent loads preserve it.
	ld	a,(_kos_syscall_result_c)
	rra
	ld	a,(_kos_syscall_result_a)
	ld	hl,(_kos_syscall_result_hl)
	ret
