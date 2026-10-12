; -----------------------------------------------------------------------------
; KonaOS nucleus-layout target-side self-test (build option only).
;
; Linked only in `make nucleus-selftest` builds.  Exercises what this
; package changes under revision 13:
;
;   - the syscall gate with P != 0 on entry: the smoke caller selects the
;     driver page in mode 10 and issues SYS_DIAG_SYSTEM_INFO, whose body
;     is cold-half code.  Only a gate that clears P can dispatch it; the
;     caller then verifies the deterministic record and the exact restore
;     of the latch it selected (page field included);
;   - the IRQ gate with P != 0: the caller enables interrupts with the
;     driver page selected.  The cold-half handler reads the latch itself
;     and records what it saw (P = 0, mode 11, the interrupted bank) and
;     where it ran (the fixed-core ISR stack); after RETI the caller's
;     latch is exactly the one it selected;
;   - the upcall shim (kernel/memory/upcall.asm): the fixed-core
;     `kos_nucleus_stub_install` writes a stub into bank 6 through the
;     window page, and the cold test driver then calls kos_upcall, which
;     must select page 1, reach the bank-6 stub, publish its result and
;     restore the latch.
;
; The smoke caller is a position-independent blob (jr only for internal
; control flow; every external reference is a fixed common address), so it
; can be copied into an application bank's caller window.
;
; Placement: the harness that crosses modes lives in permanent common
; memory; the blob, the bank-6 stub and the install helper live in the
; fixed core (the install helper's continuation must survive the page
; selection); the state the caller and the handler share lives in common
; memory so both modes reach it.
; -----------------------------------------------------------------------------

	.module nucleus_selftest
	.include "hardware.inc"
	.include "memory.inc"
	.include "nucleus_entry.inc"

	.globl _kos_nucleus_selftest_run
	.globl kos_nucleus_selftest_done
	.globl _kos_nucleus_result
	.globl _kos_nucleus_bank
	.globl kos_nucleus_kernel_sp
	.globl _kos_nucleus_irq_evidence
	.globl _kos_nucleus_irq_latch
	.globl _kos_nucleus_phase
	.globl _kos_nucleus_upcall_irq_evidence
	.globl _kos_nucleus_upcall_irq_latch
	.globl kos_nucleus_p_latch
	.globl kos_nucleus_timeout
	.globl kos_nucleus_blob_start
	.globl kos_nucleus_blob_end
	.globl kos_nucleus_rst30_stub_bytes
	.globl kos_nucleus_expected_record
	.globl kos_nucleus_stub_start
	.globl kos_nucleus_stub_end
	.globl _kos_nucleus_stub_install
	.globl _kos_nucleus_sp_is_isr
	.globl _kos_nucleus_upcall_arg
	.globl _kos_nucleus_stub_latch
	.globl _kos_nucleus_stub_args
	.globl _kos_nucleus_stub_witness
	.globl kos_nucleus_upcall_latch0
	; Kernel-owned symbols this file only reads or calls.
	.globl __kos_mode_fault
	.globl _platform_memctl_read
	.globl kos_gate_stack_top
	.globl kos_syscall_entry
	.globl _irq_active_source

	.area _COMMON

; ---------------------------------------------------------------------------
; _kos_nucleus_selftest_run - C-callable driver entry.  A = application
; bank (0-6).  Copies the RST 30h stub and the smoke caller into that
; bank's window, then enters it in mode 10.  Does not return directly: the
; smoke caller finishes via kos_nucleus_selftest_done, which pops this
; routine's return address and lands back in the C driver.
; Clobbers: AF, BC, DE, HL.
; ---------------------------------------------------------------------------
_kos_nucleus_selftest_run::
	and	#KOS_BANK_MASK
	cp	#KOS_BANK_MASK
	jp	z,__kos_mode_fault
	; Remember the kernel SP and the test bank.
	ld	hl,#0x0000
	add	hl,sp
	ld	(kos_nucleus_kernel_sp),hl
	ld	(_kos_nucleus_bank),a
	; Mode 11 over the test bank: 0000h-1FFFh shows that bank while the
	; fixed core stays visible at 2000h-5FFFh, so both copies below are
	; reachable at once.
	or	#KOS_MODE_OS
	out	(KOS_MEMCTL_PORT),a
	; Install the three-byte RST 30h stub into the bank's page zero.
	ld	hl,#kos_nucleus_rst30_stub_bytes
	ld	de,#0x0030
	ld	bc,#0x0003
	ldir
	; Copy the smoke caller into the bank's caller window.
	ld	hl,#kos_nucleus_blob_start
	ld	de,#KOS_NS_CALLER_BASE
	ld	bc,#(kos_nucleus_blob_end - kos_nucleus_blob_start)
	ldir
	; Application mode over the test bank with the driver page selected;
	; the next instructions are common code, and the jump target below is
	; visible in mode 10 (the far aperture 8000h-BFFFh stays unused).
	ld	a,#(KOS_MODE_APPLICATION | KOS_NS_PAGE_DRIVER << KOS_ROM_PAGE_SHIFT)
	ld	b,a
	ld	a,(_kos_nucleus_bank)
	or	b
	out	(KOS_MEMCTL_PORT),a
	jp	KOS_NS_CALLER_BASE

; ---------------------------------------------------------------------------
; kos_nucleus_selftest_done - common continuation reached by the smoke
; caller in mode 10 after recording its result.  Switches SP off the
; disappearing test stack, re-enters OS mode over the test bank with
; P = 0, restores the kernel SP, and returns to the C driver that called
; kos_nucleus_selftest_run.
; Clobbers: AF, HL; SP restored to the kernel stack.
; ---------------------------------------------------------------------------
kos_nucleus_selftest_done::
	ld	sp,#kos_gate_stack_top
	in	a,(KOS_MEMCTL_PORT)
	and	#KOS_BANK_MASK
	or	#KOS_MODE_OS
	out	(KOS_MEMCTL_PORT),a
	ld	sp,(kos_nucleus_kernel_sp)
	ret

; ---------------------------------------------------------------------------
; _kos_nucleus_sp_is_isr - uint8_t result in L: 1 when the current SP lies
; inside the fixed-core ISR stack (5F00h-5FFFh).  Called from the
; registered handler to prove handler placement after the relocation.
; Clobbers: AF, HL.
; ---------------------------------------------------------------------------
_kos_nucleus_sp_is_isr::
	ld	hl,#0x0000
	add	hl,sp
	ld	a,h
	cp	#>KOS_IRQ_ISR_STACK_BASE
	jr	nz,100$
	ld	a,#0x01
	ld	l,a
	ret
100$:
	xor	a
	ld	l,a
	ret

	.area _COMMON_DATA

; State the smoke caller (mode 10), the handler (mode 11) and the C driver
; all reach: permanent common memory is mapped in both modes.
kos_nucleus_kernel_sp::
	.ds	2
_kos_nucleus_bank::
	.ds	1
_kos_nucleus_result::
	.ds	1
_kos_nucleus_irq_evidence::
	.ds	1
_kos_nucleus_irq_latch::
	.ds	1
; The delivery phase the handler reports on: 1 = the smoke caller's own
; window (mode 10 with the driver page selected), 2 = the upcall shim call
; (mode 11 with the window on bank 6).
_kos_nucleus_phase::
	.ds	1
_kos_nucleus_upcall_irq_evidence::
	.ds	1
_kos_nucleus_upcall_irq_latch::
	.ds	1
kos_nucleus_p_latch::
	.ds	1
kos_nucleus_timeout::
	.ds	2
; The record the successful diagnostic syscall must have left in the
; caller's argument block.
kos_nucleus_expected_record::
	.db	KOS_NS_SYSTEM_INFO_SIZE, KOS_NS_SYSTEM_INFO_ABI
	.db	KOS_NS_MODE_APPLICATION, KOS_NS_MODE_OS
	.db	KOS_NS_COMMON_LO, KOS_NS_COMMON_HI
	.db	KOS_NS_KERNEL_BANK, KOS_NS_ROMFS_PAGE

	.area _CODE

; Page-zero stub bytes the harness copies into the application bank.
kos_nucleus_rst30_stub_bytes::
	.db	#0xC3, #<kos_syscall_entry, #>kos_syscall_entry

; ---------------------------------------------------------------------------
; Smoke caller blob (copied to KOS_NS_CALLER_BASE in the test bank).
;
; Runs in mode 10 over the test bank with the driver page selected.  Only
; jr is used for control flow: the blob is position independent.
; ---------------------------------------------------------------------------
kos_nucleus_blob_start::
	ld	sp,#KOS_NS_TEST_STACK	; stack in this bank, below 8000h
	xor	a
	ld	(_irq_active_source),a	; nothing armed before phase I
	ld	(_kos_nucleus_result),a
	ld	(_kos_nucleus_irq_evidence),a
	ld	(_kos_nucleus_irq_latch),a
	; ================= phase S: syscall with P != 0 =================
	ld	a,#(KOS_MODE_APPLICATION | KOS_NS_APP_BANK | KOS_NS_PAGE_DRIVER << KOS_ROM_PAGE_SHIFT)
	ld	(kos_nucleus_p_latch),a
	out	(KOS_MEMCTL_PORT),a
	ld	a,#KOS_NS_SYSCALL_NUM
	ld	hl,#KOS_NS_ARGBLOCK
	rst	0x30
	jr	c,zn_fail_syscall_carry
	ld	a,h
	or	a
	jr	nz,zn_fail_syscall_result
	ld	a,l
	cp	#KOS_NS_SYSCALL_RESULT
	jr	nz,zn_fail_syscall_result
	ld	hl,#KOS_NS_ARGBLOCK
	ld	de,#kos_nucleus_expected_record
	ld	b,#KOS_NS_SYSTEM_INFO_SIZE
zn_record:
	ld	a,(de)
	cp	(hl)
	jr	nz,zn_fail_syscall_record
	inc	hl
	inc	de
	djnz	zn_record
	in	a,(KOS_MEMCTL_PORT)
	ld	b,a
	ld	a,(kos_nucleus_p_latch)
	cp	b
	jr	nz,zn_fail_syscall_latch
	; ================== phase I: IRQ with P != 0 ====================
	ld	a,#KOS_NS_PHASE_GATES
	ld	(_kos_nucleus_phase),a
	ld	a,#0xFF
	ld	(_irq_active_source),a
	xor	a
	ld	(kos_nucleus_timeout),a
	ld	(kos_nucleus_timeout + 1),a
	ei
zn_wait:
	ld	a,(_irq_active_source)
	or	a
	jr	z,zn_irq_delivered
	ld	a,(kos_nucleus_timeout)
	add	a,#0x01
	ld	(kos_nucleus_timeout),a
	jr	nz,zn_wait
	ld	a,(kos_nucleus_timeout + 1)
	add	a,#0x01
	ld	(kos_nucleus_timeout + 1),a
	jr	nz,zn_wait
	di
	jr	zn_fail_irq_timeout
zn_irq_delivered:
	di
	in	a,(KOS_MEMCTL_PORT)
	ld	b,a
	ld	a,(kos_nucleus_p_latch)
	cp	b
	jr	nz,zn_fail_irq_latch
	ld	a,(_kos_nucleus_irq_latch)
	cp	#(KOS_MODE_OS | KOS_NS_APP_BANK)
	jr	nz,zn_fail_irq_handler
	ld	a,(_kos_nucleus_irq_evidence)
	cp	#(KOS_NS_EV_DELIVERED | KOS_NS_EV_PAGE_ZERO | KOS_NS_EV_BANK | KOS_NS_EV_ISR_STACK)
	jr	nz,zn_fail_irq_evidence
	xor	a
	jr	zn_finish
	; ===================== failure paths ============================
zn_fail_syscall_carry:
	ld	a,#KOS_NS_FAIL_SYSCALL_CARRY
	jr	zn_finish
zn_fail_syscall_result:
	ld	a,#KOS_NS_FAIL_SYSCALL_RESULT
	jr	zn_finish
zn_fail_syscall_record:
	ld	a,#KOS_NS_FAIL_SYSCALL_RECORD
	jr	zn_finish
zn_fail_syscall_latch:
	ld	a,#KOS_NS_FAIL_SYSCALL_LATCH
	jr	zn_finish
zn_fail_irq_timeout:
	ld	a,#KOS_NS_FAIL_IRQ_TIMEOUT
	jr	zn_finish
zn_fail_irq_latch:
	ld	a,#KOS_NS_FAIL_IRQ_LATCH
	jr	zn_finish
zn_fail_irq_handler:
	ld	a,#KOS_NS_FAIL_IRQ_HANDLER
	jr	zn_finish
zn_fail_irq_evidence:
	ld	a,#KOS_NS_FAIL_IRQ_EVIDENCE
zn_finish:
	ld	(_kos_nucleus_result),a
	jp	kos_nucleus_selftest_done
kos_nucleus_blob_end::

; ---------------------------------------------------------------------------
; kos_nucleus_stub_start/end - the upcall target.  These bytes are copied
; into bank 6 at KOS_NS_STUB_BASE and executed with P = 1, so they may only
; touch fixed-core or common memory (both stay visible whatever the window
; shows) and use no stack beyond the caller's.
;
; The stub records the latch it was called with (the selected page), the
; argument pointer the shim passed in HL, and a witness byte proving these
; instructions really ran; its HL result is the value the shim publishes.
; ---------------------------------------------------------------------------
kos_nucleus_stub_start::
	in	a,(KOS_MEMCTL_PORT)
	ld	(_kos_nucleus_stub_latch),a
	ld	(_kos_nucleus_stub_args),hl
	ld	a,#KOS_NS_STUB_WITNESS
	ld	(_kos_nucleus_stub_witness),a

	; The delivery that matters for the gate is the one this stub takes
	; itself: an interrupt while mode 11 shows bank 6.  Arm the source
	; cell, enable interrupts and wait for the CTC0 stub to clear it.
	ld	a,#KOS_NS_PHASE_UPCALL
	ld	(_kos_nucleus_phase),a
	ld	a,#0xFF
	ld	(_irq_active_source),a
	xor	a
	ld	(kos_nucleus_timeout),a
	ld	(kos_nucleus_timeout + 1),a
	ei
zs_wait:
	ld	a,(_irq_active_source)
	or	a
	jr	z,zs_delivered
	ld	a,(kos_nucleus_timeout)
	add	a,#0x01
	ld	(kos_nucleus_timeout),a
	jr	nz,zs_wait
	ld	a,(kos_nucleus_timeout + 1)
	add	a,#0x01
	ld	(kos_nucleus_timeout + 1),a
	jr	nz,zs_wait
	di
	ld	a,#KOS_NS_STUB_STATUS_TIMEOUT
	jr	zs_record
zs_delivered:
	di
	ld	a,#KOS_NS_STUB_STATUS_DELIVERED
zs_record:
	ld	(_kos_nucleus_stub_status),a
	; The gate must have restored the page this stub was called with.
	in	a,(KOS_MEMCTL_PORT)
	ld	(_kos_nucleus_stub_latch_after),a
	ld	hl,#KOS_NS_STUB_MAGIC
	ret
kos_nucleus_stub_end::

; ---------------------------------------------------------------------------
; _kos_nucleus_stub_install - uint8_t result in A (always 0).  Selects the
; driver page from the fixed core, copies the stub above into bank 6
; through the window, and restores the caller's exact latch.  Records that
; latch so the caller can prove the restore.
; Clobbers: AF, BC, DE, HL.
; ---------------------------------------------------------------------------
_kos_nucleus_stub_install::
	in	a,(KOS_MEMCTL_PORT)
	ld	(kos_nucleus_upcall_latch0),a
	and	#(KOS_MODE_MASK | KOS_BANK_MASK)
	or	#(KOS_NS_PAGE_DRIVER << KOS_ROM_PAGE_SHIFT)
	out	(KOS_MEMCTL_PORT),a
	ld	hl,#kos_nucleus_stub_start
	ld	de,#KOS_NS_STUB_BASE
	ld	bc,#(kos_nucleus_stub_end - kos_nucleus_stub_start)
	ldir
	ld	a,(kos_nucleus_upcall_latch0)
	out	(KOS_MEMCTL_PORT),a
	xor	a
	ld	l,a
	ret

	.area _DATA

; Fixed-core upcall state: visible to the shim, to the bank-6 stub and to
; the cold test driver.  Single-depth, like the shim itself.
kos_nucleus_upcall_latch0::
	.ds	1
_kos_nucleus_upcall_arg::
	.ds	2
_kos_nucleus_stub_latch::
	.ds	1
_kos_nucleus_stub_args::
	.ds	2
_kos_nucleus_stub_witness::
	.ds	1
_kos_nucleus_stub_status::
	.ds	1
_kos_nucleus_stub_latch_after::
	.ds	1
