; -----------------------------------------------------------------------------
; ZephyrOS common-resident IPC aperture copy (kernel/ipc, correction 01 to
; architecture 36.8).
;
; Path B of the copy resolver.  The caller buffer lies in the caller's own
; bank (P = 0 when the descriptor was captured) and the driver buffer lies
; in bank 6's 8000h-BFFFh.  Neither the mode-11 window nor the mode-10
; primary decode shows both, so the primitive runs in mode 10 with
; Q = the caller's bank and P = 1: the aperture 8000h-BFFFh then shows
; bank 6 at the address the driver itself names, and everything else
; shows the caller.  One LDIR moves the run.
;
; Everything executed while that latch is in force must be fetched from
; common memory: bank 7 (the fixed core) is not mapped in mode 10.  This
; file is therefore entirely common, and the primitive is register-only -
; no CALL, no push, no banked operand - so the only memory it touches
; open is the request/state block below.  It never reads or writes the
; driver stack either: SP is moved to the small common reservation here
; for the length of the window.
;
; The copy is interruptible on purpose.  A CTC0 interrupt taken inside the
; window pushes onto that common reservation, never onto the driver stack
; (fixed core, unmapped in mode 10), and the IRQ gate restores the
; interrupted latch exactly - mode, Q and P included - before its RETI
; (kernel/irq/entry.asm saves the full latch and normalizes only for
; dispatch).  The window is therefore bounded by the descriptor length,
; never by an interrupt mask.
;
; Interface (kernel-internal, not a native ABI).  The resolver fills
; `ipc_ap_req` in mode 11, where the block is ordinary common data:
;
;   [0]     latch to install: mode 10 | caller bank | page 1
;   [1..2]  source address (visible under that latch)
;   [3..4]  destination address (visible under that latch)
;   [5..6]  length in bytes (already bounded by the resolver)
;
;   uint16_t ipc_ap_copy(void);
;       returns the length in DE.  Single-depth: only the resolver calls
;       it, and the resolver runs one copy at a time.
; -----------------------------------------------------------------------------

	.module ipc_common
	.include "hardware.inc"
	.include "memory.inc"

	.globl _ipc_ap_copy
	.globl _ipc_ap_req, _ipc_ap_saved_latch, _ipc_ap_saved_sp
	.globl ipc_common_begin, ipc_common_end

	.area _COMMON

ipc_common_begin::

; ---------------------------------------------------------------------------
; uint16_t ipc_ap_copy(void);
;
; Entry: SP on the driver stack (mode 11), the request filled.  Exit: the
; caller's latch restored, SP back on the driver stack, length in DE.
; ---------------------------------------------------------------------------
_ipc_ap_copy::
	in	a,(ZOS_MEMCTL_PORT)	; the latch to come back to
	ld	(_ipc_ap_saved_latch),a
	ld	(_ipc_ap_saved_sp),sp
	ld	sp,#_ipc_ap_stack_top	; common: visible in mode 10, and
					; the only place an ISR may push
	ld	a,(_ipc_ap_req)
	ld	hl,(_ipc_ap_req+1)	; LDIR moves (HL) -> (DE)
	ld	de,(_ipc_ap_req+3)
	ld	bc,(_ipc_ap_req+5)
	out	(ZOS_MEMCTL_PORT),a	; aperture opens: bank 6 at 8000h
	ldir
	ld	a,(_ipc_ap_saved_latch)
	out	(ZOS_MEMCTL_PORT),a	; restore before SP leaves common
	ld	sp,(_ipc_ap_saved_sp)
	ld	de,(_ipc_ap_req+5)
	ret

ipc_common_end::

; ---------------------------------------------------------------------------
; Common state.  Fixed for the whole copy, and all of it must be common:
; the saved latch and the request are read while mode 10 is in force, and
; the reservation below is where an interrupt taken in that window pushes.
; ---------------------------------------------------------------------------
	.area _COMMON_DATA
_ipc_ap_req::
	.ds	7
_ipc_ap_saved_latch::
	.ds	1
_ipc_ap_saved_sp::
	.ds	2
_ipc_ap_stack::
	.ds	ZOS_IPC_APERTURE_STACK_SIZE
_ipc_ap_stack_top::
