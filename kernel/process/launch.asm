; -----------------------------------------------------------------------------
; ZephyrOS process launch/resume gates (common-resident, package 28).
;
; The kernel-side lifecycle (kernel/process/process.c, exec.c) runs in mode
; 11; entering the child and coming back both change the memory mapping, so
; every instruction here is permanent common memory.
;
; The state in _COMMON_DATA below is permanent common memory for the same
; reason: the resume gate copies the saved gate frame back and restores
; SP/latch/IFF *while* it is switching the mapping, so it must reach that
; state from whatever mapping is live at each step.  Common memory is not
; isolated from mode 10 (there is no protection; an application bank sees
; it), so this is reachability, not safety: state that is only touched in
; mode 11 belongs in bank 7 (`_zos_process_path_scratch` is in
; kernel/process/process.c for exactly that reason).
;
; Package 28 generalizes the M6 single-depth launcher into a bounded
; continuation stack indexed by nesting depth (ZOS_EXEC_MAX_DEPTH levels):
;
;   index 0        kernel launcher on the boot stack (no gate frame)
;   index 1..3     a user parent suspended in its SYS_EXEC handler; the
;                  parent's gate-stack frame is copied to frame save
;                  (index - 1) at launch and restored at resume
;
; The common launch gate saves the launcher's SP/latch/IFF into
; zos_proc_cont/zos_proc_latch/zos_proc_iff[index], copies the gate frame
; ([SP, zos_gate_stack_top)) into zos_proc_frames[index-1] when the parent
; is a user process, switches to mode 10 over the child bank and RETs into
; the child entry with the common return trap beneath it.
;
; The syscall gate's transition state (zos_gate_saved_sp/latch/iff) is
; single-depth: every syscall of the running process overwrites it.  A
; nested exec therefore shadows the launcher's copy at the same depth
; (zos_proc_gate_*) on launch and puts it back on resume, so the resuming
; process returns through the gate with its own stack pointer, bank and
; interrupt state instead of the child's.  Depth 0 is the kernel launcher,
; which never returns through the gate and needs no shadow.
;
; Launch:
;   zos_process_launch(req)   HL = const struct zos_process_launch_req *
;     layout (packed): +0 bank (0-6), +1..2 entry, +3 index
;
; Resume:
;   zos_process_resume(req)   HL = const struct zos_process_resume_req *
;     layout (packed): +0..1 child exit status, +2 index
;     Called from bank-7 teardown code with interrupts disabled.  Discards
;     its own return address: the restored SP points at the launch call
;     site's return address, and the RET below resumes the launcher there.
;
; The whole resume path from the start of the LDIR until SP is restored
; performs no CALL, PUSH, POP or interrupt enable: every value it needs
; (saved SP, saved latch, saved IFF, frame source and the child's exit
; status) is already in registers or common state before the copy begins.
; -----------------------------------------------------------------------------

	.module process_launch
	.include "zephyr80/hardware.inc"
	.include "zephyr80/memory.inc"

	.globl _zos_process_launch
	.globl _zos_process_resume
	.globl _zos_process_continuation_size
	.globl zos_proc_gate_sp
	.globl zos_proc_gate_latch
	.globl zos_proc_gate_iff
	.globl zos_process_return_trap
	.globl _zos_process_page_zero_vector
	.globl zos_proc_cont
	.globl zos_proc_ix
	.globl zos_proc_latch
	.globl zos_proc_iff
	.globl zos_proc_frames
	.globl zos_proc_frames_end
	.globl zos_gate_saved_sp
	.globl zos_gate_saved_latch
	.globl zos_gate_saved_iff
	.globl zos_proc_index
	.globl zos_proc_status
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
; Entry: mode 11 (kernel context), HL = launch request.
; ---------------------------------------------------------------------------
_zos_process_launch::
	; Read the request before any state change.  Bank 7 is not a
	; process bank.
	ld	a,(hl)
	cp	#ZOS_BANK_MASK
	jp	nc,__zos_mode_fault
	ld	(zos_proc_bank),a
	inc	hl
	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	ld	(zos_proc_entry),de
	inc	hl
	ld	a,(hl)
	ld	(zos_proc_index),a

	; Snapshot IFF2 before DI clears it, then mask IRQs.
	ld	a,i
	di
	ld	a,#0
	jp	po,001$
	inc	a
001$:
	ld	(zos_proc_iff_tmp),a

	; Capture the launch call site's SP before any push.  This is the
	; continuation resume point (it points at the return address into
	; the C launch wrapper).
	ld	hl,#0
	add	hl,sp
	ld	(zos_proc_sp_tmp),hl

	; Save the launcher latch; reject a caller that is not in OS mode.
	in	a,(ZOS_MEMCTL_PORT)
	ld	(zos_proc_latch_tmp),a
	and	#ZOS_MODE_MASK
	cp	#ZOS_MODE_OS
	jp	nz,__zos_mode_fault

	; saved_latch[index] = latch
	ld	hl,#zos_proc_latch
	ld	a,(zos_proc_index)
	ld	e,a
	ld	d,#0
	add	hl,de
	ld	a,(zos_proc_latch_tmp)
	ld	(hl),a

	; saved_iff[index] = iff
	ld	hl,#zos_proc_iff
	ld	a,(zos_proc_index)
	ld	e,a
	ld	d,#0
	add	hl,de
	ld	a,(zos_proc_iff_tmp)
	ld	(hl),a

	; Shadow the single-depth syscall-gate transition state.  The
	; child's syscalls overwrite zos_gate_saved_*, so a user launcher
	; keeps its own copy at the depth of its continuation.  Depth 0 is
	; the kernel launcher: it has no gate state and no child of its own
	; to shadow against.
	ld	a,(zos_proc_index)
	or	a
	jr	z,002$

	; gate_latch[index] = zos_gate_saved_latch
	ld	hl,#zos_proc_gate_latch
	ld	a,(zos_proc_index)
	ld	e,a
	ld	d,#0
	add	hl,de
	ld	a,(zos_gate_saved_latch)
	ld	(hl),a

	; gate_iff[index] = zos_gate_saved_iff
	ld	hl,#zos_proc_gate_iff
	ld	a,(zos_proc_index)
	ld	e,a
	ld	d,#0
	add	hl,de
	ld	a,(zos_gate_saved_iff)
	ld	(hl),a

	; gate_sp[index] = zos_gate_saved_sp
	ld	hl,(zos_gate_saved_sp)
	ld	(zos_proc_gate_tmp),hl
	ld	hl,#zos_proc_gate_sp
	ld	a,(zos_proc_index)
	add	a,a			; 2 * index
	ld	e,a
	ld	d,#0
	add	hl,de
	ld	a,(zos_proc_gate_tmp)
	ld	(hl),a
	inc	hl
	ld	a,(zos_proc_gate_tmp+1)
	ld	(hl),a
002$:

	; saved_sp[index] = captured SP
	ld	hl,#zos_proc_cont
	ld	a,(zos_proc_index)
	add	a,a			; 2 * index
	ld	e,a
	ld	d,#0
	add	hl,de
	ld	a,(zos_proc_sp_tmp)
	ld	(hl),a
	inc	hl
	ld	a,(zos_proc_sp_tmp+1)
	ld	(hl),a

	; saved_ix[index] = IX.  IX is the C launcher's frame pointer and is
	; callee-saved; the child clobbers it, so the resume must restore it
	; before RET or the C epilogue's SP=IX would use a stale value.
	ld	hl,#zos_proc_ix
	ld	a,(zos_proc_index)
	add	a,a
	ld	e,a
	ld	d,#0
	add	hl,de
	push	ix
	pop	de
	ld	(hl),e
	inc	hl
	ld	(hl),d

	; Depth 0 (the kernel launcher) needs no frame copy: its stack is the
	; boot stack, which the child never touches.  A user parent's gate
	; frame is copied to zos_proc_frames[index-1].
	ld	a,(zos_proc_index)
	or	a
	jr	z,003$
	dec	a			; frame area 0..ZOS_EXEC_MAX_DEPTH - 2
	; HL = a * ZOS_EXEC_FRAME_SAVE.  The factor is a shift chain
	; (a + 2a = 3a, then four doublings = 48a) because the Z80 has no
	; multiply; ZOS_EXEC_FRAME_SAVE in include/zephyros/process.h is
	; pinned against this chain by tests/test_m9_exec.py.
	ld	l,a
	ld	h,#0
	ld	d,h
	ld	e,l			; DE = a
	add	hl,hl			; 2a
	add	hl,de			; 3a
	add	hl,hl			; 6a
	add	hl,hl			; 12a
	add	hl,hl			; 24a
	add	hl,hl			; 48a = a * ZOS_EXEC_FRAME_SAVE
	ld	de,#zos_proc_frames
	add	hl,de			; HL = frame destination
	push	hl			; save destination
	ld	hl,#zos_proc_cont
	ld	a,(zos_proc_index)
	add	a,a
	ld	e,a
	ld	d,#0
	add	hl,de
	ld	e,(hl)
	inc	hl
	ld	d,(hl)			; DE = saved SP
	push	de			; save source
	ld	hl,#zos_gate_stack_top
	or	a
	sbc	hl,de			; HL = gate stack bytes in use
	ld	b,h
	ld	c,l			; BC = length
	pop	hl			; HL = source
	pop	de			; DE = destination
	ldir
003$:
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

	; ZEX v1 startup block v1 lives at 0080h in the child page zero;
	; the program is entered with HL = 0080h.  A program that ignores
	; HL is unaffected.  The RET enters the child with interrupts
	; enabled; the trap address stays on the stack.
	ld	hl,#0x0080
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
; Entry: mode 11 (bank-7 teardown), interrupts disabled, HL = resume
; request.  Never returns to its caller.
; ---------------------------------------------------------------------------
_zos_process_resume::
	di
	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	inc	hl
	ld	a,(hl)
	ld	(zos_proc_index),a
	ld	(zos_proc_status),de

	; IX = saved_sp[index]
	ld	hl,#zos_proc_cont
	ld	a,(zos_proc_index)
	add	a,a
	ld	e,a
	ld	d,#0
	add	hl,de
	ld	a,(hl)
	inc	hl
	ld	h,(hl)
	ld	l,a
	push	hl
	pop	ix

	; Restore the gate frame for a user parent (index >= 1).  The LDIR
	; begins the stackless window: nothing below touches the stack until
	; SP is restored.
	ld	a,(zos_proc_index)
	or	a
	jr	z,004$
	dec	a			; frame area 0..ZOS_EXEC_MAX_DEPTH - 2
	; HL = a * ZOS_EXEC_FRAME_SAVE.  The factor is a shift chain
	; (a + 2a = 3a, then four doublings = 48a) because the Z80 has no
	; multiply; ZOS_EXEC_FRAME_SAVE in include/zephyros/process.h is
	; pinned against this chain by tests/test_m9_exec.py.
	ld	l,a
	ld	h,#0
	ld	d,h
	ld	e,l			; DE = a
	add	hl,hl			; 2a
	add	hl,de			; 3a
	add	hl,hl			; 6a
	add	hl,hl			; 12a
	add	hl,hl			; 24a
	add	hl,hl			; 48a = a * ZOS_EXEC_FRAME_SAVE
	ld	de,#zos_proc_frames
	add	hl,de			; HL = frame source
	push	hl			; save source
	push	ix
	pop	bc			; BC = saved SP
	ld	hl,#zos_gate_stack_top
	or	a
	sbc	hl,bc			; HL = bytes to restore
	ld	b,h
	ld	c,l			; BC = length
	pop	hl			; HL = source
	push	ix
	pop	de			; DE = destination (saved SP)
	ldir

004$:
	; Restore the launcher latch (no stack access).
	ld	hl,#zos_proc_latch
	ld	a,(zos_proc_index)
	ld	e,a
	ld	d,#0
	add	hl,de
	ld	a,(hl)
	out	(ZOS_MEMCTL_PORT),a

	; Put the resumed process's own syscall-gate transition state back;
	; the child's last syscalls left theirs in the common single-depth
	; copy.  Still inside the stackless window (plain loads and stores).
	ld	a,(zos_proc_index)
	or	a
	jr	z,006$

	; zos_gate_saved_latch = gate_latch[index]
	ld	hl,#zos_proc_gate_latch
	ld	a,(zos_proc_index)
	ld	e,a
	ld	d,#0
	add	hl,de
	ld	a,(hl)
	ld	(zos_gate_saved_latch),a

	; zos_gate_saved_iff = gate_iff[index]
	ld	hl,#zos_proc_gate_iff
	ld	a,(zos_proc_index)
	ld	e,a
	ld	d,#0
	add	hl,de
	ld	a,(hl)
	ld	(zos_gate_saved_iff),a

	; zos_gate_saved_sp = gate_sp[index]
	ld	hl,#zos_proc_gate_sp
	ld	a,(zos_proc_index)
	add	a,a			; 2 * index
	ld	e,a
	ld	d,#0
	add	hl,de
	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	ld	(zos_gate_saved_sp),de
006$:

	; Restore the launcher SP; the next RET returns to the launch call
	; site.  From here to RET there is no stack access.
	ld	sp,ix

	; Restore IX to the C launcher's frame pointer so its epilogue is
	; valid.  LD IX,(nn) needs the value in common memory first.
	ld	hl,#zos_proc_ix
	ld	a,(zos_proc_index)
	add	a,a
	ld	e,a
	ld	d,#0
	add	hl,de
	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	ld	(zos_proc_ix_tmp),de
	ld	ix,(zos_proc_ix_tmp)

	ld	hl,(zos_proc_status)
	or	a			; carry clear: SYS_EXEC success

	; Restore the launcher IFF exactly.  User parents were suspended by
	; the syscall gate with interrupts masked, so this is only set for
	; the kernel launcher.
	ld	a,(zos_proc_index)
	ld	e,a
	ld	d,#0
	ld	hl,#zos_proc_iff
	add	hl,de
	ld	a,(hl)
	or	a
	jr	z,005$
	ei
005$:
	ld	hl,(zos_proc_status)
	or	a
	ret

; ---------------------------------------------------------------------------
; _zos_process_continuation_size - gate-stack bytes in use by the caller.
;
; C name zos_process_continuation_size.  Returns HL = zos_gate_stack_top
; minus the caller's SP (the caller's frame on the gate stack).  Called
; from the same SYS_EXEC frame for the preflight and immediately before
; the launch, so the two measurements must match.
; ---------------------------------------------------------------------------
_zos_process_continuation_size::
	ld	hl,#0
	add	hl,sp
	inc	hl
	inc	hl			; caller SP (undo the call's push)
	ex	de,hl			; DE = caller SP
	ld	hl,#zos_gate_stack_top
	or	a
	sbc	hl,de			; HL = gate_top - caller SP
	ex	de,hl			; SDCC 16-bit return is in DE
	ret

	.area _COMMON_DATA

; The three bytes every process page zero carries at 0030h.  Emitted as
; data so the loader can copy one deterministic vector into any bank
; (C name zos_process_page_zero_vector).
_zos_process_page_zero_vector::
	.db	0xC3			; JP zos_syscall_entry
	.dw	zos_syscall_entry

; Continuation state (common RAM, kernel-lifetime).  The C side selects a
; depth and reads/writes only through these arrays.  All of it is read by
; the resume gate inside the stackless window, between the frame LDIR and
; the latch restore, while the mapping is still the child's.
zos_proc_cont::
	.ds	8			; saved SP per depth (2 * ZOS_EXEC_MAX_DEPTH)
zos_proc_ix::
	.ds	8			; saved IX (C frame pointer) per depth
zos_proc_latch::
	.ds	4			; saved latch per depth
zos_proc_iff::
	.ds	4			; saved IFF2 per depth
zos_proc_index::
	.ds	1			; active depth for the primitive
zos_proc_status::
	.ds	2			; child status carried into resume
zos_proc_gate_sp::
	.ds	8			; shadowed gate caller SP per depth
zos_proc_gate_latch::
	.ds	4			; shadowed gate caller latch per depth
zos_proc_gate_iff::
	.ds	4			; shadowed gate caller IFF per depth
zos_proc_gate_tmp::
	.ds	2
zos_proc_iff_tmp::
	.ds	1
zos_proc_latch_tmp::
	.ds	1
zos_proc_sp_tmp::
	.ds	2
zos_proc_ix_tmp::
	.ds	2
zos_proc_bank::
	.ds	1
zos_proc_entry::
	.ds	2
; ZOS_EXEC_MAX_DEPTH - 1 gate-frame save areas of ZOS_EXEC_FRAME_SAVE
; bytes each (index i + 1 uses area i).  zos_proc_frames_end pins the
; exact size for mapcheck; ZOS_EXEC_FRAME_SAVE is owned by
; include/zephyros/process.h.
zos_proc_frames::
	.ds	144
zos_proc_frames_end::
