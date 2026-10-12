; -----------------------------------------------------------------------------
; KonaOS syscall-gate target-side self-test (build option only).
;
; Linked only in `make syscall-selftest` builds.  The smoke caller is a
; position-independent blob in bank 7 that kos_selftest_run copies into an
; application bank window together with the RST 30h page-zero stub, then
; executes in mode 10.  It issues RST 30h, verifies the whole ABI contract
; (error result, preserved registers, restored SP/mode/bank), records the
; outcome in common state, and re-enters the kernel through
; kos_selftest_done.
;
; Only jr is used for control flow inside the blob; every external
; reference is a fixed common/bank-7 address, so the blob is relocatable
; to KOS_ST_CALLER_BASE in any application bank.
; -----------------------------------------------------------------------------

	.module selftest
	.include "hardware.inc"
	.include "memory.inc"
	.include "syscall_entry.inc"

	.globl _kos_selftest_run
	.globl kos_selftest_done
	.globl _kos_selftest_result
	.globl _kos_selftest_bank
	.globl kos_selftest_kernel_sp
	.globl kos_selftest_blob_start
	.globl kos_selftest_blob_end
	.globl kos_rst30_stub_bytes
	.globl __kos_mode_fault
	.globl _platform_memctl_read
	.globl kos_gate_stack_top
	.globl kos_syscall_entry

	.area _COMMON

; ---------------------------------------------------------------------------
; _kos_selftest_run - C-callable driver entry.  A = application bank (0-6).
; Does not return directly: the smoke caller finishes via
; kos_selftest_done, which pops the return address of this routine's own
; call and lands back in the C driver.
; Clobbers: AF, BC, DE, HL.
; ---------------------------------------------------------------------------
_kos_selftest_run::
	and	#KOS_BANK_MASK
	cp	#KOS_BANK_MASK
	jp	z,__kos_mode_fault
	; Remember the kernel SP (common stack) and the test bank.
	ld	hl,#0x0000
	add	hl,sp
	ld	(kos_selftest_kernel_sp),hl
	ld	(_kos_selftest_bank),a
	; Mode 11 over the test bank: 0000h-1FFFh now shows that bank while
	; bank 7 stays visible at 2000h-DFFFh, so source and destination of
	; both copies are reachable at once.
	or	#KOS_MODE_OS
	out	(KOS_MEMCTL_PORT),a
	; Install the three-byte RST 30h stub into the bank's page zero.
	ld	hl,#kos_rst30_stub_bytes
	ld	de,#0x0030
	ld	bc,#0x0003
	ldir
	; Copy the smoke caller into the bank's caller window.
	ld	hl,#kos_selftest_blob_start
	ld	de,#KOS_ST_CALLER_BASE
	ld	bc,#(kos_selftest_blob_end - kos_selftest_blob_start)
	ldir
	; Application mode over the test bank; the next instructions are
	; common code, and the jump target below is visible in mode 10.
	ld	a,(_kos_selftest_bank)
	or	#KOS_MODE_APPLICATION
	out	(KOS_MEMCTL_PORT),a
	jp	KOS_ST_CALLER_BASE

; ---------------------------------------------------------------------------
; kos_selftest_done - common continuation reached by the smoke caller in
; mode 10 after recording its result.  Switches SP off the disappearing
; test stack, re-enters OS mode, restores the kernel SP, and returns to
; the C driver that called kos_selftest_run.
; Clobbers: AF, HL; SP restored to the kernel stack.
; ---------------------------------------------------------------------------
kos_selftest_done::
	ld	sp,#kos_gate_stack_top
	in	a,(KOS_MEMCTL_PORT)
	and	#KOS_BANK_MASK
	or	#KOS_MODE_OS
	out	(KOS_MEMCTL_PORT),a
	ld	sp,(kos_selftest_kernel_sp)
	ret

	.area _COMMON_DATA

kos_selftest_kernel_sp::
	.ds	2
_kos_selftest_bank::
	.ds	1
_kos_selftest_result::
	.ds	1

	.area _CODE

; Page-zero stub bytes the driver copies into the application bank.
kos_rst30_stub_bytes::
	.db	#0xC3, #<kos_syscall_entry, #>kos_syscall_entry

; ---------------------------------------------------------------------------
; Smoke caller blob (copied to KOS_ST_CALLER_BASE in the test bank).
; ---------------------------------------------------------------------------
kos_selftest_blob_start::
	ld	sp,#KOS_ST_TEST_STACK	; stack above 2000h: hidden in mode 11
	ld	bc,#KOS_ST_BC_SENTINEL
	ld	de,#KOS_ST_DE_SENTINEL
	ld	ix,#KOS_ST_IX_SENTINEL
	ld	iy,#KOS_ST_IY_SENTINEL
	ld	a,#KOS_ST_SYSCALL_NUM
	ld	hl,#KOS_ST_ARGBLOCK
	rst	0x30
	; ---- return contract ----
	jr	c,zt_eno
zt_fail_carry:
	ld	a,#KOS_ST_FAIL_CARRY
	ld	(_kos_selftest_result),a
	jp	kos_selftest_done
zt_eno:
	cp	#KOS_ENOSYS
	jr	z,zt_result
zt_fail_errno:
	ld	a,#KOS_ST_FAIL_ERRNO
	ld	(_kos_selftest_result),a
	jp	kos_selftest_done
zt_result:
	ld	a,h
	or	l
	jr	z,zt_bc
zt_fail_result:
	ld	a,#KOS_ST_FAIL_RESULT
	ld	(_kos_selftest_result),a
	jp	kos_selftest_done
	; ---- preserved registers ----
zt_bc:
	ld	a,b
	cp	#>KOS_ST_BC_SENTINEL
	jr	nz,zt_fail_bc
	ld	a,c
	cp	#<KOS_ST_BC_SENTINEL
	jr	nz,zt_fail_bc
zt_de:
	ld	a,d
	cp	#>KOS_ST_DE_SENTINEL
	jr	nz,zt_fail_de
	ld	a,e
	cp	#<KOS_ST_DE_SENTINEL
	jr	nz,zt_fail_de
zt_ix:
	push	ix
	pop	hl
	ld	a,h
	cp	#>KOS_ST_IX_SENTINEL
	jr	nz,zt_fail_ix
	ld	a,l
	cp	#<KOS_ST_IX_SENTINEL
	jr	nz,zt_fail_ix
zt_iy:
	push	iy
	pop	hl
	ld	a,h
	cp	#>KOS_ST_IY_SENTINEL
	jr	nz,zt_fail_iy
	ld	a,l
	cp	#<KOS_ST_IY_SENTINEL
	jr	nz,zt_fail_iy
	; ---- restored SP ----
zt_sp:
	ld	hl,#0x0000
	add	hl,sp
	ld	a,h
	cp	#>KOS_ST_TEST_STACK
	jr	nz,zt_fail_sp
	ld	a,l
	cp	#<KOS_ST_TEST_STACK
	jr	nz,zt_fail_sp
	; ---- restored mode/bank (BC/DE now free) ----
zt_latch:
	call	_platform_memctl_read	; L = latch (call uses the test stack)
	ld	b,l
	ld	a,b
	and	#KOS_MODE_MASK
	cp	#KOS_MODE_APPLICATION
	jr	nz,zt_fail_mode
	ld	a,(_kos_selftest_bank)
	ld	c,a
	ld	a,b
	and	#KOS_BANK_MASK
	cp	c
	jr	nz,zt_fail_bank
	; ---- success ----
	xor	a
	ld	(_kos_selftest_result),a
	jp	kos_selftest_done

zt_fail_bc:
	ld	a,#KOS_ST_FAIL_BC
	ld	(_kos_selftest_result),a
	jp	kos_selftest_done
zt_fail_de:
	ld	a,#KOS_ST_FAIL_DE
	ld	(_kos_selftest_result),a
	jp	kos_selftest_done
zt_fail_ix:
	ld	a,#KOS_ST_FAIL_IX
	ld	(_kos_selftest_result),a
	jp	kos_selftest_done
zt_fail_iy:
	ld	a,#KOS_ST_FAIL_IY
	ld	(_kos_selftest_result),a
	jp	kos_selftest_done
zt_fail_sp:
	ld	a,#KOS_ST_FAIL_SP
	ld	(_kos_selftest_result),a
	jp	kos_selftest_done
zt_fail_mode:
	ld	a,#KOS_ST_FAIL_MODE
	ld	(_kos_selftest_result),a
	jp	kos_selftest_done
zt_fail_bank:
	ld	a,#KOS_ST_FAIL_BANK
	ld	(_kos_selftest_result),a
	jp	kos_selftest_done

kos_selftest_blob_end::
