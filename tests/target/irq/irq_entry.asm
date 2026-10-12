; -----------------------------------------------------------------------------
; KonaOS interrupt-executive target-side self-test (build option only).
;
; Linked only in `make irq-selftest` builds.  Exercises the kernel-owned
; IM2 executive end to end:
;
;   - a position-independent smoke caller runs in application mode 10 over
;     banks 0 and 6 with a stack above 2000h and enables interrupts;
;   - three CTC0 interrupts are delivered per bank, including one through a
;     HALT window so the restored AF can be verified bit-exactly;
;   - full main/alternate register state, SP, mode, and bank identity are
;     verified after delivery;
;   - the syscall gate is exercised with interrupts enabled and its IFF
;     restore is verified with LD A,I;
;   - a mode-11 helper verifies delivery and restoration on the kernel
;     stack, including the unclaimed-source path.
;
; All test state lives in permanent common memory so both modes can read
; and write it.  Nothing here prints from IRQ context.
;
; The smoke caller is position independent (jr only for internal control
; flow; external references are fixed common/bank-7 addresses), so it can
; be copied to KOS_IST_CALLER_BASE in any application bank.
; -----------------------------------------------------------------------------

	.module irq_selftest
	.include "hardware.inc"
	.include "memory.inc"
	.include "irq_entry.inc"

	.globl _kos_irq_selftest_run
	.globl kos_irq_selftest_done
	.globl _kos_irq_test_mode11
	.globl kos_irq_test_mode11_end
	.globl _kos_irq_test_sp_is_isr
	.globl _kos_irq_test_result
	.globl _kos_irq_test_bank
	.globl _kos_irq_test_delivered
	.globl _kos_irq_test_evidence
	.globl kos_irq_test_kernel_sp
	.globl kos_irq_test_timeout
	.globl kos_irq_test_blob_start
	.globl kos_irq_test_blob_end
	.globl __kos_mode_fault
	.globl kos_syscall_entry
	.globl _platform_memctl_read
	.globl kos_gate_stack_top
	.globl _irq_active_source

	.area _COMMON

; ---------------------------------------------------------------------------
; _kos_irq_selftest_run - C-callable driver entry.  A = application bank
; (0-6).  Copies the smoke caller and an RST 30h stub into the bank's
; caller window, then jumps to it in mode 10.  Does not return directly:
; the smoke caller finishes via kos_irq_selftest_done, which pops this
; routine's return address and lands back in the C driver.
; Clobbers: AF, BC, DE, HL.
; ---------------------------------------------------------------------------
_kos_irq_selftest_run::
	and	#KOS_BANK_MASK
	cp	#KOS_BANK_MASK
	jp	z,__kos_mode_fault
	; Remember the kernel SP and the test bank.
	ld	hl,#0x0000
	add	hl,sp
	ld	(kos_irq_test_kernel_sp),hl
	ld	(_kos_irq_test_bank),a
	; Mode 11 over the test bank: 0000h-1FFFh shows that bank while bank 7
	; stays visible at 2000h-DFFFh, so both copies are reachable at once.
	or	#KOS_MODE_OS
	out	(KOS_MEMCTL_PORT),a
	; Install the three-byte RST 30h stub into the bank's page zero.
	ld	hl,#kos_irq_rst30_stub_bytes
	ld	de,#0x0030
	ld	bc,#0x0003
	ldir
	; Copy the smoke caller into the bank's caller window.
	ld	hl,#kos_irq_test_blob_start
	ld	de,#KOS_IST_CALLER_BASE
	ld	bc,#(kos_irq_test_blob_end - kos_irq_test_blob_start)
	ldir
	; Application mode over the test bank; the next instructions are
	; common code, and the jump target below is visible in mode 10.
	ld	a,(_kos_irq_test_bank)
	or	#KOS_MODE_APPLICATION
	out	(KOS_MEMCTL_PORT),a
	jp	KOS_IST_CALLER_BASE

; ---------------------------------------------------------------------------
; kos_irq_selftest_done - common continuation reached by the smoke caller
; in mode 10 after recording its result.  Switches SP off the disappearing
; test stack, re-enters OS mode, restores the kernel SP, and returns to
; the C driver that called kos_irq_selftest_run.
; Clobbers: AF, HL; SP restored to the kernel stack.
; ---------------------------------------------------------------------------
kos_irq_selftest_done::
	ld	sp,#kos_gate_stack_top
	in	a,(KOS_MEMCTL_PORT)
	and	#KOS_BANK_MASK
	or	#KOS_MODE_OS
	out	(KOS_MEMCTL_PORT),a
	ld	sp,(kos_irq_test_kernel_sp)
	ret

; ---------------------------------------------------------------------------
; _kos_irq_test_sp_is_isr - uint8_t result in L.  Returns 1 when the
; current SP lies inside the bank-7 ISR stack DF00h-DFFFh.  Called from
; the registered handler to prove handler placement on the ISR stack.
; Clobbers: AF, HL.
; ---------------------------------------------------------------------------
_kos_irq_test_sp_is_isr::
	ld	hl,#0x0000
	add	hl,sp
	ld	a,h
	cp	#>KOS_IRQ_ISR_STACK_BASE
	jr	nz,100$
	ld	l,#0x01
	ret
100$:
	ld	l,#0x00
	ret

	.area _COMMON_DATA

kos_irq_test_kernel_sp::
	.ds	2
kos_irq_test_timeout::
	.ds	2
_kos_irq_test_bank::
	.ds	1
_kos_irq_test_result::
	.ds	1
_kos_irq_test_delivered::
	.ds	1
_kos_irq_test_evidence::
	.ds	1

	.area _CODE

; Page-zero stub bytes the driver copies into the application bank.
kos_irq_rst30_stub_bytes::
	.db	#0xC3, #<kos_syscall_entry, #>kos_syscall_entry

; ---------------------------------------------------------------------------
; Smoke caller blob (copied to KOS_IST_CALLER_BASE in the test bank).
;
; Runs in mode 10 with a stack above 2000h.  Enables maskable interrupts
; and waits for three CTC0 deliveries:
;
;   windows 1-2  EI + bounded source-cell spin (A-only; every other
;                register stays at its sentinel value for the final check)
;   window 3     EI + HALT: the interrupt wakes the CPU with a
;                deterministic flag state, so the restored AF can be
;                verified bit-exactly (masked to the documented flag bits)
;
; After delivery the smoke caller verifies the full main and alternate
; register state, SP, the mode/bank mapping, then stops CTC0 and probes
; the syscall gate with interrupts enabled to prove the IFF restore.
; ---------------------------------------------------------------------------
kos_irq_test_blob_start::
	ld	sp,#KOS_IST_TEST_STACK	; stack above 2000h: hidden in mode 11
	; ---- main sentinels ----
	ld	bc,#KOS_IST_BC_SENTINEL
	ld	de,#KOS_IST_DE_SENTINEL
	ld	hl,#KOS_IST_HL_SENTINEL
	ld	ix,#KOS_IST_IX_SENTINEL
	ld	iy,#KOS_IST_IY_SENTINEL
	; ---- alternate sentinels ----
	ex	af,af'
	ld	a,#KOS_IST_ALT_A_SENTINEL
	exx
	ld	bc,#KOS_IST_ALT_BC_SENTINEL
	ld	de,#KOS_IST_ALT_DE_SENTINEL
	ld	hl,#KOS_IST_ALT_HL_SENTINEL
	exx
	ex	af,af'
	; ---- reset the test cells ----
	xor	a
	ld	(_kos_irq_test_delivered),a
	ld	(_kos_irq_test_evidence),a
	; ================= delivery window 1 =================
	ld	a,#0xFF
	ld	(_irq_active_source),a
	ld	a,#0x00
	ld	(kos_irq_test_timeout),a
	ld	(kos_irq_test_timeout + 1),a
	ei
zt_w1:
	ld	a,(_irq_active_source)
	cp	#0x00
	jr	z,zt_w1_ok
	ld	a,(kos_irq_test_timeout)
	add	a,#0x01
	ld	(kos_irq_test_timeout),a
	jr	nz,zt_w1
	ld	a,(kos_irq_test_timeout + 1)
	add	a,#0x01
	ld	(kos_irq_test_timeout + 1),a
	jr	nz,zt_w1
	di
	ld	a,#KOS_IST_FAIL_TIMEOUT
	ld	(_kos_irq_test_result),a
	jp	kos_irq_selftest_done
zt_w1_ok:
	di
	; ================= delivery window 2 =================
	ld	a,#0xFF
	ld	(_irq_active_source),a
	ld	a,#0x00
	ld	(kos_irq_test_timeout),a
	ld	(kos_irq_test_timeout + 1),a
	ei
zt_w2:
	ld	a,(_irq_active_source)
	cp	#0x00
	jr	z,zt_w2_ok
	ld	a,(kos_irq_test_timeout)
	add	a,#0x01
	ld	(kos_irq_test_timeout),a
	jr	nz,zt_w2
	ld	a,(kos_irq_test_timeout + 1)
	add	a,#0x01
	ld	(kos_irq_test_timeout + 1),a
	jr	nz,zt_w2
	di
	ld	a,#KOS_IST_FAIL_TIMEOUT
	ld	(_kos_irq_test_result),a
	jp	kos_irq_selftest_done
zt_w2_ok:
	di
	; ============ delivery window 3: HALT + exact AF restore ============
	ld	a,#0xFF
	ld	(_irq_active_source),a
	ld	a,#KOS_IST_A_SENTINEL
	or	a			; deterministic flags: S=0 Z=0 H=0 PV=1 N=0 C=0
	ei
	halt
	; The interrupt wakes the CPU; execution resumes here after RETI.
	di
	push	hl
	push	af
	ld	a,(_irq_active_source)
	cp	#0x00
	jr	z,zt_w3_source_ok
	pop	af
	pop	hl
	jr	zt_fail_repeat
zt_w3_source_ok:
	; AF was restored bit-exactly: check A and the documented flag bits.
	pop	hl
	ld	a,h
	cp	#KOS_IST_A_SENTINEL
	jr	nz,zt_fail_af_saved_hl
	ld	a,l
	and	#0xD7			; documented flag bits only
	cp	#0x04			; PV set by `or a` on 0x55
	jr	nz,zt_fail_af_saved_hl
	pop	hl
	; repeated delivery: three handler runs must be recorded
	ld	a,(_kos_irq_test_delivered)
	sub	#0x03
	jr	nc,zt_regcheck
zt_fail_repeat:
	ld	a,#KOS_IST_FAIL_REPEAT
	ld	(_kos_irq_test_result),a
	jp	kos_irq_selftest_done
zt_fail_af_saved_hl:
	pop	hl
zt_fail_af:
	ld	a,#KOS_IST_FAIL_AF
	ld	(_kos_irq_test_result),a
	jp	kos_irq_selftest_done
	; ---- full main-register verification (interrupts disabled) ----
zt_regcheck:
	ld	a,b
	cp	#>KOS_IST_BC_SENTINEL
	jr	nz,zt_fail_bc
	ld	a,c
	cp	#<KOS_IST_BC_SENTINEL
	jr	nz,zt_fail_bc
	jr	zt_after_bc
zt_fail_bc:
	ld	a,#KOS_IST_FAIL_BC
	ld	(_kos_irq_test_result),a
	jp	kos_irq_selftest_done
zt_after_bc:
	ld	a,d
	cp	#>KOS_IST_DE_SENTINEL
	jr	nz,zt_fail_de
	ld	a,e
	cp	#<KOS_IST_DE_SENTINEL
	jr	nz,zt_fail_de
	jr	zt_after_de
zt_fail_de:
	ld	a,#KOS_IST_FAIL_DE
	ld	(_kos_irq_test_result),a
	jp	kos_irq_selftest_done
zt_after_de:
	ld	a,h
	cp	#>KOS_IST_HL_SENTINEL
	jr	nz,zt_fail_hl
	ld	a,l
	cp	#<KOS_IST_HL_SENTINEL
	jr	nz,zt_fail_hl
	jr	zt_after_hl
zt_fail_hl:
	ld	a,#KOS_IST_FAIL_HL
	ld	(_kos_irq_test_result),a
	jp	kos_irq_selftest_done
zt_after_hl:
	push	ix
	pop	hl
	ld	a,h
	cp	#>KOS_IST_IX_SENTINEL
	jr	nz,zt_fail_ix
	ld	a,l
	cp	#<KOS_IST_IX_SENTINEL
	jr	nz,zt_fail_ix
	jr	zt_after_ix
zt_fail_ix:
	ld	a,#KOS_IST_FAIL_IX
	ld	(_kos_irq_test_result),a
	jp	kos_irq_selftest_done
zt_after_ix:
	push	iy
	pop	hl
	ld	a,h
	cp	#>KOS_IST_IY_SENTINEL
	jr	nz,zt_fail_iy
	ld	a,l
	cp	#<KOS_IST_IY_SENTINEL
	jr	nz,zt_fail_iy
	jr	zt_after_iy
zt_fail_iy:
	ld	a,#KOS_IST_FAIL_IY
	ld	(_kos_irq_test_result),a
	jp	kos_irq_selftest_done
zt_after_iy:
	; ---- SP ----
	ld	hl,#0x0000
	add	hl,sp
	ld	a,h
	cp	#>KOS_IST_TEST_STACK
	jr	nz,zt_fail_sp
	ld	a,l
	cp	#<KOS_IST_TEST_STACK
	jr	nz,zt_fail_sp
	jr	zt_after_sp
zt_fail_sp:
	ld	a,#KOS_IST_FAIL_SP
	ld	(_kos_irq_test_result),a
	jp	kos_irq_selftest_done
zt_after_sp:
	; ---- mode 10 over the test bank ----
	call	_platform_memctl_read	; L = latch (call uses the test stack)
	ld	a,l
	and	#KOS_MODE_MASK
	cp	#KOS_MODE_APPLICATION
	jr	nz,zt_fail_mode
	ld	a,l
	and	#KOS_BANK_MASK
	ld	c,a
	ld	a,(_kos_irq_test_bank)
	cp	c
	jr	nz,zt_fail_bank
	jr	zt_after_latch
zt_fail_mode:
	ld	a,#KOS_IST_FAIL_MODE
	ld	(_kos_irq_test_result),a
	jp	kos_irq_selftest_done
zt_fail_bank:
	ld	a,#KOS_IST_FAIL_BANK
	ld	(_kos_irq_test_result),a
	jp	kos_irq_selftest_done
zt_after_latch:
	; ---- alternate registers ----
	ex	af,af'
	cp	#KOS_IST_ALT_A_SENTINEL
	jr	nz,zt_fail_alt_af
	exx
	ld	a,b
	cp	#>KOS_IST_ALT_BC_SENTINEL
	jr	nz,zt_fail_alt_bc
	ld	a,c
	cp	#<KOS_IST_ALT_BC_SENTINEL
	jr	nz,zt_fail_alt_bc
	ld	a,d
	cp	#>KOS_IST_ALT_DE_SENTINEL
	jr	nz,zt_fail_alt_de
	ld	a,e
	cp	#<KOS_IST_ALT_DE_SENTINEL
	jr	nz,zt_fail_alt_de
	ld	a,h
	cp	#>KOS_IST_ALT_HL_SENTINEL
	jr	nz,zt_fail_alt_hl
	ld	a,l
	cp	#<KOS_IST_ALT_HL_SENTINEL
	jr	nz,zt_fail_alt_hl
	exx
	ex	af,af'
	jr	zt_after_alts
zt_fail_alt_af:
	ld	a,#KOS_IST_FAIL_ALT_AF
	ld	(_kos_irq_test_result),a
	jp	kos_irq_selftest_done
zt_fail_alt_bc:
	ld	a,#KOS_IST_FAIL_ALT_BC
	ld	(_kos_irq_test_result),a
	jp	kos_irq_selftest_done
zt_fail_alt_de:
	ld	a,#KOS_IST_FAIL_ALT_DE
	ld	(_kos_irq_test_result),a
	jp	kos_irq_selftest_done
zt_fail_alt_hl:
	ld	a,#KOS_IST_FAIL_ALT_HL
	ld	(_kos_irq_test_result),a
	jp	kos_irq_selftest_done
zt_after_alts:
	; ---- quiesce CTC0 before the IFF-restore probe ----
	ld	a,#KOS_CTC0_RESET
	out	(KOS_CTC0_PORT),a
	; ---- syscall gate must restore the prior EI state ----
	; No interrupt source is active now, so the EI window is race-free.
	ld	a,#KOS_IST_SYSCALL_NUM
	ld	hl,#KOS_IST_ARGBLOCK
	ei
	rst	0x30
	; Back in mode 10.  The gate must have returned carry set and the
	; ENOSYS result; only then may the IFF probe clobber A with LD A,I.
	jr	nc,zt_fail_syscall
	cp	#KOS_IST_ENOSYS
	jr	nz,zt_fail_syscall
	ld	a,h
	or	l
	jr	nz,zt_fail_syscall
	ld	a,i
	push	af
	pop	hl
	bit	#2,l			; PV = IFF2 restored by the gate
	jr	z,zt_fail_iff
	di
	; ---- success ----
	xor	a
	ld	(_kos_irq_test_result),a
	jp	kos_irq_selftest_done
zt_fail_iff:
	ld	a,#KOS_IST_FAIL_IFF
	ld	(_kos_irq_test_result),a
	jp	kos_irq_selftest_done
zt_fail_syscall:
	ld	a,#KOS_IST_FAIL_SYSCALL
	ld	(_kos_irq_test_result),a
	jp	kos_irq_selftest_done

kos_irq_test_blob_end::

; ---------------------------------------------------------------------------
; _kos_irq_test_mode11 - C-callable mode-11 delivery check.
;
; Called by the C driver while mode 11 is active over the kernel stack.
; Arms the source cell, sets deterministic main/alternate sentinels,
; enables interrupts, and HALTs; the unclaimed CTC0 interrupt wakes the
; CPU and the executive must restore the full prior mode-11 context.
; The CTC0 was proven alive by the smoke caller, so a dead device cannot
; hang this path indefinitely (it would have failed the blob's timeouts).
;
; Records the outcome in _kos_irq_test_result and returns.
; Clobbers: AF, BC, DE, HL.
; ---------------------------------------------------------------------------
_kos_irq_test_mode11::
	; Capture the kernel SP (the CALL already pushed the return address).
	ld	hl,#0x0000
	add	hl,sp
	ld	(kos_irq_test_kernel_sp),hl
	; ---- main + alternate sentinels ----
	ld	bc,#KOS_IST_BC_SENTINEL
	ld	de,#KOS_IST_DE_SENTINEL
	ld	hl,#KOS_IST_HL_SENTINEL
	ld	ix,#KOS_IST_IX_SENTINEL
	ld	iy,#KOS_IST_IY_SENTINEL
	ld	a,#KOS_IST_A_SENTINEL
	ex	af,af'
	ld	a,#KOS_IST_ALT_A_SENTINEL
	exx
	ld	bc,#KOS_IST_ALT_BC_SENTINEL
	ld	de,#KOS_IST_ALT_DE_SENTINEL
	ld	hl,#KOS_IST_ALT_HL_SENTINEL
	exx
	ex	af,af'
	; ---- arm and wait for the unclaimed delivery ----
	ld	a,#0xFF
	ld	(_irq_active_source),a
	ld	a,#KOS_IST_A_SENTINEL
	or	a			; deterministic flags (PV=1)
	ei
	halt
	; Resumed here after RETI, back in the prior mode-11 context.
	di
	push	hl
	push	af
	ld	a,(_irq_active_source)
	cp	#0x00
	jr	z,zt_m11_source_ok
	pop	af
	pop	hl
	jp	zt_m11_fail_timeout
zt_m11_source_ok:
zt_m11_af:
	; AF restored bit-exactly (masked to the documented flag bits).
	pop	hl
	ld	a,h
	cp	#KOS_IST_A_SENTINEL
	jp	nz,zt_m11_fail_af_saved_hl
	ld	a,l
	and	#0xD7
	cp	#0x04
	jp	nz,zt_m11_fail_af_saved_hl
	pop	hl
	; ---- main registers ----
	ld	a,b
	cp	#>KOS_IST_BC_SENTINEL
	jp	nz,zt_m11_fail_bc
	ld	a,c
	cp	#<KOS_IST_BC_SENTINEL
	jp	nz,zt_m11_fail_bc
	ld	a,d
	cp	#>KOS_IST_DE_SENTINEL
	jp	nz,zt_m11_fail_de
	ld	a,e
	cp	#<KOS_IST_DE_SENTINEL
	jp	nz,zt_m11_fail_de
	ld	a,h
	cp	#>KOS_IST_HL_SENTINEL
	jp	nz,zt_m11_fail_hl
	ld	a,l
	cp	#<KOS_IST_HL_SENTINEL
	jp	nz,zt_m11_fail_hl
	push	ix
	pop	hl
	ld	a,h
	cp	#>KOS_IST_IX_SENTINEL
	jp	nz,zt_m11_fail_ix
	ld	a,l
	cp	#<KOS_IST_IX_SENTINEL
	jp	nz,zt_m11_fail_ix
	push	iy
	pop	hl
	ld	a,h
	cp	#>KOS_IST_IY_SENTINEL
	jp	nz,zt_m11_fail_iy
	ld	a,l
	cp	#<KOS_IST_IY_SENTINEL
	jp	nz,zt_m11_fail_iy
	; ---- SP ----
	ld	hl,#0x0000
	add	hl,sp
	ld	de,(kos_irq_test_kernel_sp)
	ld	a,h
	cp	d
	jp	nz,zt_m11_fail_sp
	ld	a,l
	cp	e
	jp	nz,zt_m11_fail_sp
	; ---- mode must still be 11 ----
	in	a,(KOS_MEMCTL_PORT)
	and	#KOS_MODE_MASK
	cp	#KOS_MODE_OS
	jp	nz,zt_m11_fail_mode
	; ---- alternate registers ----
	ex	af,af'
	cp	#KOS_IST_ALT_A_SENTINEL
	jp	nz,zt_m11_fail_alt_af
	exx
	ld	a,b
	cp	#>KOS_IST_ALT_BC_SENTINEL
	jp	nz,zt_m11_fail_alt_bc
	ld	a,c
	cp	#<KOS_IST_ALT_BC_SENTINEL
	jp	nz,zt_m11_fail_alt_bc
	ld	a,d
	cp	#>KOS_IST_ALT_DE_SENTINEL
	jp	nz,zt_m11_fail_alt_de
	ld	a,e
	cp	#<KOS_IST_ALT_DE_SENTINEL
	jp	nz,zt_m11_fail_alt_de
	ld	a,h
	cp	#>KOS_IST_ALT_HL_SENTINEL
	jp	nz,zt_m11_fail_alt_hl
	ld	a,l
	cp	#<KOS_IST_ALT_HL_SENTINEL
	jp	nz,zt_m11_fail_alt_hl
	exx
	ex	af,af'
	; ---- success ----
	xor	a
	ld	(_kos_irq_test_result),a
	ret

zt_m11_fail_timeout:
	ld	a,#KOS_IST_FAIL_TIMEOUT
	ld	(_kos_irq_test_result),a
	ret
zt_m11_fail_af_saved_hl:
	pop	hl
zt_m11_fail_af:
	ld	a,#KOS_IST_FAIL_AF
	ld	(_kos_irq_test_result),a
	ret
zt_m11_fail_bc:
	ld	a,#KOS_IST_FAIL_BC
	ld	(_kos_irq_test_result),a
	ret
zt_m11_fail_de:
	ld	a,#KOS_IST_FAIL_DE
	ld	(_kos_irq_test_result),a
	ret
zt_m11_fail_hl:
	ld	a,#KOS_IST_FAIL_HL
	ld	(_kos_irq_test_result),a
	ret
zt_m11_fail_ix:
	ld	a,#KOS_IST_FAIL_IX
	ld	(_kos_irq_test_result),a
	ret
zt_m11_fail_iy:
	ld	a,#KOS_IST_FAIL_IY
	ld	(_kos_irq_test_result),a
	ret
zt_m11_fail_sp:
	ld	a,#KOS_IST_FAIL_SP
	ld	(_kos_irq_test_result),a
	ret
zt_m11_fail_mode:
	ld	a,#KOS_IST_FAIL_MODE
	ld	(_kos_irq_test_result),a
	ret
zt_m11_fail_alt_af:
	ld	a,#KOS_IST_FAIL_ALT_AF
	ld	(_kos_irq_test_result),a
	ret
zt_m11_fail_alt_bc:
	ld	a,#KOS_IST_FAIL_ALT_BC
	ld	(_kos_irq_test_result),a
	ret
zt_m11_fail_alt_de:
	ld	a,#KOS_IST_FAIL_ALT_DE
	ld	(_kos_irq_test_result),a
	ret
zt_m11_fail_alt_hl:
	ld	a,#KOS_IST_FAIL_ALT_HL
	ld	(_kos_irq_test_result),a
	ret

kos_irq_test_mode11_end::
