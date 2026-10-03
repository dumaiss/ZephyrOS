; -----------------------------------------------------------------------------
; ZephyrOS M6 process launch/resume gates (common-resident).
;
; The kernel-side lifecycle (kernel/process/process.c, exec.c) runs in
; mode 11; entering the child and coming back both change the memory
; mapping, so every instruction here is permanent common memory.
;
; The gate is deliberately single-depth: one launcher, one child.  The
; suspended parent's SP, latch, and IFF are saved in common state before
; the mapping changes and restored exactly on resume; the resume path
; returns through the launcher's original call site (a bank-7 code
; address that is only valid again in mode 11).
;
; Launch:
;   zos_process_launch(bank, entry)   A = bank (0-6), DE = entry address
;     (SDCC sdcccall(1): 8-bit argument in A, 16-bit argument in DE)
;     - saves the launcher context in common state
;     - switches to mode 10 over `bank`
;     - SP = E000h (top of the reserved [E000h-stack_size, E000h) stack)
;     - pushes the common return trap, then the entry address
;     - HL = 0000h (no startup block in ZEX v1), EI, RET into the entry
;   The gate never returns to its caller directly.  A raw entry return
;   pops the common trap and exits the child with ZOS_EFAULT.
;
; Resume:
;   zos_process_resume()   called from bank-7 teardown code (process_exit
;   / process_abort); restores the saved latch, SP, and IFF, then RETs to
;   the launch call site.  Never returns to its caller.
; -----------------------------------------------------------------------------

	.module process_launch
	.include "zephyr80/hardware.inc"
	.include "zephyr80/memory.inc"

	.globl _zos_process_launch
	.globl _zos_process_resume
	.globl zos_process_return_trap
	.globl _zos_process_page_zero_vector
	.globl zos_proc_saved_sp
	.globl zos_proc_saved_latch
	.globl zos_proc_saved_iff
	.globl zos_proc_bank
	.globl zos_proc_entry
	.globl zos_syscall_entry
	.globl zos_gate_stack_top
	.globl __zos_mode_fault
	.globl _process_abort

	.area _COMMON

; ---------------------------------------------------------------------------
; _zos_process_launch - enter one child process (C name zos_process_launch).
;
; Entry: mode 11 (kernel context), A = application bank 0-6, DE = entry.
; ---------------------------------------------------------------------------
_zos_process_launch::
	cp	#ZOS_BANK_MASK		; bank 7 is not a process bank
	jp	nc,__zos_mode_fault
	ld	(zos_proc_entry),de
	ld	(zos_proc_bank),a

	; Snapshot the launcher's interrupt state before DI clears IFF2.
	ld	a,i
	di
	ld	a,#0
	jp	po,001$
	inc	a
001$:
	ld	(zos_proc_saved_iff),a
	in	a,(ZOS_MEMCTL_PORT)
	ld	(zos_proc_saved_latch),a
	and	#ZOS_MODE_MASK
	cp	#ZOS_MODE_OS
	jp	nz,__zos_mode_fault
	ld	(zos_proc_saved_sp),sp

	; Enter mode 10 over the child bank; everything after this OUT is
	; fetched from common memory.
	ld	a,(zos_proc_bank)
	or	#ZOS_MODE_APPLICATION
	out	(ZOS_MEMCTL_PORT),a

	; Build the child stack: SP = E000h, trap below it, entry on top.
	; The first push lands at DFFEh-DFFFh inside the child bank, so the
	; reserved stack is [E000h-stack_size, E000h).
	ld	sp,#ZOS_COMMON_BASE
	ld	hl,#zos_process_return_trap
	push	hl
	ld	hl,(zos_proc_entry)
	push	hl

	; ZEX v1 has no startup block: HL = 0000h.  The RET enters the child
	; with interrupts enabled; the trap address stays on the stack.
	ld	hl,#0x0000
	ei
	ret

; ---------------------------------------------------------------------------
; zos_process_return_trap - a process entry returned instead of exiting.
;
; Entered via RET on the child stack in mode 10.  A raw entry return is
; a process fault: exit through the kernel teardown path with
; ZOS_EFAULT.  The switch to the common gate stack happens before the
; mapping change, and the bank-7 continuation is called only in mode 11.
; ---------------------------------------------------------------------------
zos_process_return_trap::
	di
	ld	a,(zos_proc_bank)
	or	#ZOS_MODE_OS
	ld	sp,#zos_gate_stack_top
	out	(ZOS_MEMCTL_PORT),a
	call	_process_abort
	jp	__zos_mode_fault	; process_abort does not return

; ---------------------------------------------------------------------------
; _zos_process_resume - restore the suspended launcher exactly (C name
; zos_process_resume).
;
; Called from bank-7 teardown code with interrupts disabled.  Discards
; its own return address: the saved SP points at the launch call site's
; return address, and the RET below resumes the launcher there.
; ---------------------------------------------------------------------------
_zos_process_resume::
	di
	ld	a,(zos_proc_saved_latch)
	out	(ZOS_MEMCTL_PORT),a
	ld	sp,(zos_proc_saved_sp)
	ld	a,(zos_proc_saved_iff)
	or	a
	jr	z,002$
	ei
002$:
	ret

	.area _COMMON_DATA

; The three bytes every process page zero carries at 0030h.  Emitted as
; data so the loader can copy one deterministic vector into any bank
; (C name zos_process_page_zero_vector).
_zos_process_page_zero_vector::
	.db	0xC3			; JP zos_syscall_entry
	.dw	zos_syscall_entry

; Single-depth launcher context (common RAM, kernel-lifetime).
zos_proc_saved_sp::
	.ds	2
zos_proc_saved_latch::
	.ds	1
zos_proc_saved_iff::
	.ds	1
zos_proc_bank::
	.ds	1
zos_proc_entry::
	.ds	2
