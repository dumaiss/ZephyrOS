; -----------------------------------------------------------------------------
; ZephyrOS native syscall gate and dispatch boundary.
;
; Native entry is RST 30h; page zero at 0030h holds JP zos_syscall_entry
; (see page_zero.asm).  Everything on the crossing path is in permanent
; common memory; the dispatcher boundary and the dispatcher itself are in
; bank 7.
;
; Entry state (documented ABI, docs/abi.md):
;   mode 10 active, interrupts disabled
;   A  = syscall number
;   HL = pointer to the argument block (must lie in 0000h-1FFFh)
;   SP = caller stack, possibly in 2000h-DFFFh (hidden by mode 11)
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
	.include "zephyr80/hardware.inc"
	.include "zephyr80/memory.inc"

	.globl zos_syscall_entry
	.globl zos_syscall_dispatch
	.globl __zos_mode_fault
	.globl zos_gate_saved_sp
	.globl zos_gate_saved_latch
	.globl zos_gate_result_hl
	.globl zos_gate_result_a
	.globl zos_gate_result_c
	.globl zos_gate_saved_iff
	.globl zos_gate_saved_a
	.globl zos_gate_stack_top
	.globl _kernel_syscall_dispatch
	.globl _zos_syscall_number
	.globl _zos_syscall_args
	.globl _zos_syscall_result_a
	.globl _zos_syscall_result_c
	.globl _zos_syscall_result_hl

	.area _COMMON

; ---------------------------------------------------------------------------
; zos_syscall_entry - common-memory syscall gate.
;
; An RST has already pushed the return PC on the caller's stack, which may
; be anywhere in the selected application bank and may disappear above
; 1FFFh once mode 11 is entered.  The gate therefore never pushes before
; switching stacks.
;
; Clobbers: none observable (see ABI); internally AF/HL/C/SP.
; ---------------------------------------------------------------------------
zos_syscall_entry::
	; Save A before LD A,I, then atomically snapshot IFF2 and mask IRQs.
	; An IRQ between the first store and DI cannot observe transition state:
	; no mapping or stack state has changed yet, and IRQ state is disjoint.
	ld	(zos_gate_saved_a),a
	ld	a,i
	di
	ld	a,#0
	jp	po,001$
	inc	a
001$:
	ld	(zos_gate_saved_iff),a

	; ED 73 captures SP without consuming HL or touching the caller stack.
	ld	(zos_gate_saved_sp),sp

	; Switch to the reserved common gate stack BEFORE the mapping changes,
	; then save the ABI-preserved registers before any of them is used as
	; scratch.
	ld	sp,#zos_gate_stack_top
	push	bc
	push	de
	push	ix
	push	iy
	push	hl			; argument-block pointer for the dispatcher
	ld	a,(zos_gate_saved_a)
	push	af			; syscall number for the dispatcher

	; Validate the entry state: mode 10 over an application bank 0-6.
	; Anything else takes the defined common-resident fatal path rather
	; than returning through an unsafe mapping.
	in	a,(ZOS_MEMCTL_PORT)
	ld	(zos_gate_saved_latch),a	; preserve the full caller latch
	and	#ZOS_MODE_MASK
	cp	#ZOS_MODE_APPLICATION
	jp	nz,__zos_mode_fault
	ld	a,(zos_gate_saved_latch)
	and	#ZOS_BANK_MASK
	cp	#ZOS_BANK_MASK		; bank 7 is not an application bank
	jp	z,__zos_mode_fault

	; Enter OS mode over the caller bank: only the mode bits change.
	ld	a,(zos_gate_saved_latch)
	and	#ZOS_BANK_MASK
	or	#ZOS_MODE_OS
	out	(ZOS_MEMCTL_PORT),a

	; Restore the syscall inputs; bank 7 is visible at 2000h-DFFFh.
	pop	af
	pop	hl

	; Dispatch.  Boundary contract (internal): A = syscall number,
	; HL = arg block; returns HL = primary result, A = error code,
	; carry set.
	call	zos_syscall_dispatch

	; Stash the results in common state so the return path can rebuild
	; registers exactly.
	ld	(zos_gate_result_hl),hl
	ld	(zos_gate_result_a),a
	ld	a,#0x00
	rla			; carry -> bit 0 of A (rla preserves carry)
	ld	(zos_gate_result_c),a

	; Restore the preserved registers.
	pop	iy
	pop	ix
	pop	de
	pop	bc

	; Back to application mode over the caller bank (common code; the
	; next instructions are fetched from memory mapped in both modes).
	ld	a,(zos_gate_saved_latch)
	out	(ZOS_MEMCTL_PORT),a

	; Restore the exact caller SP; the caller stack is visible again.
	ld	hl,(zos_gate_saved_sp)
	ld	sp,hl

	; Restore the prior interrupt state immediately before returning.  The
	; result setup is duplicated so testing saved IFF cannot alter carry.
	ld	a,(zos_gate_saved_iff)
	or	a
	jr	z,002$
	ld	a,(zos_gate_result_c)
	rra
	ld	a,(zos_gate_result_a)
	ld	hl,(zos_gate_result_hl)
	ei
	ret
002$:
	ld	a,(zos_gate_result_c)
	rra
	ld	a,(zos_gate_result_a)
	ld	hl,(zos_gate_result_hl)
	ret

	.area _CODE

; ---------------------------------------------------------------------------
; zos_syscall_dispatch - bank-7 boundary that converts the stable register
; ABI to the internal (private) C calling convention.
;
; In:  A = syscall number, HL = argument block pointer.
; Out: HL = primary result, A = error code; carry set on failure and
;      clear on success (the C dispatcher publishes it in
;      _zos_syscall_result_c).
; Clobbers: AF, BC, DE, HL (internal; the gate restores the ABI state).
; ---------------------------------------------------------------------------
zos_syscall_dispatch::
	ld	(_zos_syscall_number),a
	ld	(_zos_syscall_args),hl
	call	_kernel_syscall_dispatch
	; Rebuild the result: carry = bit 0 of the C result flag.  RRA moves
	; the flag bit into carry; the subsequent loads preserve it.
	ld	a,(_zos_syscall_result_c)
	rra
	ld	a,(_zos_syscall_result_a)
	ld	hl,(_zos_syscall_result_hl)
	ret
