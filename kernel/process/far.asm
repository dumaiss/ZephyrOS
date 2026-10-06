; -----------------------------------------------------------------------------
; ZephyrOS M10 far-call gate (common-resident, package 33d).
;
; SYS_FAR_CALL switches from the primary execution bank to a loaded
; auxiliary segment, runs a declared far entry in mode 10 with a stack in
; the auxiliary low window, and returns to the SYS_FAR_CALL handler.  The
; far code may issue ordinary syscalls; the gate therefore reuses the
; package-28 continuation idea: it saves the dispatcher's gate-stack
; frame and shadows the single-depth syscall transition state, so the far
; code's syscalls cannot destroy the dispatcher's continuation.
;
; Interface (kernel-internal):
;
;   struct zos_far_entry_req {      layout (packed):
;       uint8_t  bank;              +0  target auxiliary bank (0-6)
;       uint16_t entry;             +1  logical far entry address
;       uint16_t frame;             +3  auxiliary low-window frame base
;   };
;   void zos_far_entry(const struct zos_far_entry_req *req);  HL = req
;
; It does not return normally: the far entry RETs to a common trap that
; restores the dispatcher's frame, latch, SP and IFF and returns to the
; handler's call site.  The far entry's 16-bit result is captured in
; zos_far_result before the trap clobbers HL.
;
; Everything here is permanent common memory because it runs while the
; mapping changes.  The state below is common RAM for the same reason.
; -----------------------------------------------------------------------------

	.module far_entry
	.include "zephyr80/hardware.inc"
	.include "zephyr80/memory.inc"

	.globl _zos_far_entry
	.globl _zos_exec_bank
	.globl zos_far_return_trap
	.globl _zos_far_result
	.globl zos_gate_saved_sp
	.globl zos_gate_saved_latch
	.globl zos_gate_saved_iff
	.globl zos_gate_stack_top
	.globl __zos_mode_fault

	.area _COMMON

ZOS_FAR_STACK_TOP = 0x1F00

; ---------------------------------------------------------------------------
; _zos_far_entry - enter a far entry (C name zos_far_entry).
; ---------------------------------------------------------------------------
_zos_far_entry::
	ld	a,(hl)			; target bank
	cp	#ZOS_BANK_MASK
	jp	nc,__zos_mode_fault
	ld	(zos_far_bank),a
	inc	hl
	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	ld	(zos_far_entry_ptr),de
	inc	hl
	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	ld	(zos_far_frame_ptr),de

	; Snapshot IFF2 before DI clears it.
	ld	a,i
	di
	ld	a,#0
	jp	po,001$
	inc	a
001$:
	ld	(zos_far_saved_iff),a

	; Capture the dispatcher's SP before any push (the continuation).
	ld	hl,#0
	add	hl,sp
	ld	(zos_far_saved_sp),hl

	; Save the launcher latch; require OS mode.
	in	a,(ZOS_MEMCTL_PORT)
	ld	(zos_far_saved_latch),a
	and	#ZOS_MODE_MASK
	cp	#ZOS_MODE_OS
	jp	nz,__zos_mode_fault

	; Shadow the single-depth syscall transition state.
	ld	a,(zos_gate_saved_latch)
	ld	(zos_far_gate_latch),a
	ld	a,(zos_gate_saved_iff)
	ld	(zos_far_gate_iff),a
	ld	hl,(zos_gate_saved_sp)
	ld	(zos_far_gate_sp),hl

	; Copy the dispatcher's gate frame [saved_sp, gate_top) into
	; zos_far_frame, exactly as the launch gate does per depth.
	ld	hl,#zos_gate_stack_top
	ld	de,(zos_far_saved_sp)
	or	a
	sbc	hl,de			; HL = bytes in use
	ld	b,h
	ld	c,l			; BC = length
	ld	hl,(zos_far_saved_sp)	; HL = source
	ld	de,#zos_far_frame
	ldir

	; Enter mode 10 over the auxiliary bank; the rest is common code.
	ld	a,(zos_far_bank)
	or	#ZOS_MODE_APPLICATION
	out	(ZOS_MEMCTL_PORT),a

	; Far stack in the auxiliary low window: trap under the entry, frame
	; pointer in HL, interrupts enabled for the far code.
	ld	sp,#ZOS_FAR_STACK_TOP
	ld	hl,#zos_far_return_trap
	push	hl
	ld	hl,(zos_far_entry_ptr)
	push	hl
	ld	hl,(zos_far_frame_ptr)
	ei
	ret

; ---------------------------------------------------------------------------
; zos_far_return_trap - the far entry returned.
;
; Entered via RET on the far stack in mode 10.  HL holds the far entry's
; result.  Restores the dispatcher's frame, latch, SP and IFF, then RETs
; into the SYS_FAR_CALL handler.
; ---------------------------------------------------------------------------
zos_far_return_trap::
	ld	(_zos_far_result),de	; SDCC 16-bit return is in DE
	di
	; Back to OS mode over bank 7 before touching bank-7 state.
	ld	a,#0x1F			; ZOS_MODE_OS | bank 7
	out	(ZOS_MEMCTL_PORT),a
	ld	sp,#zos_gate_stack_top

	; Restore the dispatcher's frame (stackless window begins here).
	ld	hl,#zos_far_frame
	ld	de,(zos_far_saved_sp)
	push	de			; destination
	push	hl			; source
	ld	hl,#zos_gate_stack_top
	ld	de,(zos_far_saved_sp)
	or	a
	sbc	hl,de
	ld	b,h
	ld	c,l			; BC = length
	pop	hl			; source
	pop	de			; destination
	ldir

	; Put the dispatcher's own syscall-gate transition state back.
	ld	a,(zos_far_gate_latch)
	ld	(zos_gate_saved_latch),a
	ld	a,(zos_far_gate_iff)
	ld	(zos_gate_saved_iff),a
	ld	hl,(zos_far_gate_sp)
	ld	(zos_gate_saved_sp),hl

	; Restore the dispatcher's latch and SP, then its IFF, then RET.
	ld	a,(zos_far_saved_latch)
	out	(ZOS_MEMCTL_PORT),a
	ld	hl,(zos_far_saved_sp)
	ld	sp,hl
	ld	a,(zos_far_saved_iff)
	or	a
	jr	z,002$
	ei
002$:
	ret

; ---------------------------------------------------------------------------
; _zos_exec_bank - the caller's execution bank from the syscall gate.
;
; The gate records the caller's full latch (mode + bank) before switching
; to mode 11; the low three bits are the execution bank.  Used by the
; dispatcher so user-buffer copies follow the bank the caller actually
; runs in (the M10 execution-bank audit).
; ---------------------------------------------------------------------------
_zos_exec_bank::
	ld	a,(zos_gate_saved_latch)
	and	#ZOS_BANK_MASK
	ret

	.area _COMMON_DATA

zos_far_bank::
	.ds	1
zos_far_entry_ptr::
	.ds	2
zos_far_frame_ptr::
	.ds	2
zos_far_saved_sp::
	.ds	2
zos_far_saved_latch::
	.ds	1
zos_far_saved_iff::
	.ds	1
zos_far_gate_latch::
	.ds	1
zos_far_gate_iff::
	.ds	1
zos_far_gate_sp::
	.ds	2
_zos_far_result::
	.ds	2
zos_far_frame::
	.ds	48
