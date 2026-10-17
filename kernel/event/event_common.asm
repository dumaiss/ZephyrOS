; -----------------------------------------------------------------------------
; KonaOS device-event delivery core (kernel/event, architecture 4.5/4.10).
;
; An application subscribes to events on a bound device handle (SYS_NOTIFY)
; and the nucleus delivers them signal-style: the driver keeps its interrupt
; and only calls event_post(); the delivery happens when the process the
; event belongs to is running its own code again, never while it is inside a
; syscall, an IPC call or another handler.
;
; Everything here is common memory, and it has to be.  The delivery and the
; unwind run with the *application's* latch in force (mode 10 over its bank),
; where bank 7 - fixed core and cold half alike - is mapped only by accident.
; The state below is read from driver-interrupt context too (event_post),
; where the window shows bank 6 and the bank-7 data area is unmapped.
;
; The frame is 24 bytes on the program's own stack, from the new SP upward:
;
;   +0   latch, 0        the exact latch the interruption saved (page too)
;   +2   HL', DE', BC', AF'     the alternate set
;   +10  IY, IX, HL, DE, BC, AF the main set
;   +22  PC                     the resume address
;
; so the unwind pops it in reverse - the (latch, 0) pair, the three alternate
; pairs, AF', then the main set - and returns to the PC the frame holds.  SP
; is exactly what the interrupted code had: the frame was built *below* it.
;
; The frame must never be written where the application's own latch does not
; show it: the program stack is a mode-10 address, so it may not sit in the
; 8000h-BFFFh aperture (docs/sdk.md; the SDK's stack reservation and the far
; link rule keep it below it).
;
; Three entry points:
;
;   kos_event_irq_exit()      IRQ exit, after the interrupted latch is
;                             restored and before the transition stack is
;                             popped (the saved registers are still there)
;   kos_event_syscall_exit()  syscall exit, after the caller latch and SP are
;                             restored and before the result is rebuilt
;   kos_event_return()        the SYS_EVENT_RETURN body (cold half): either
;                             delivers again through the same frame or
;                             unwinds it
;
; The two exit checks return when nothing is deliverable and never return
; when something is: the delivery leaves through RETI to the libkos
; trampoline, which calls the handler and re-enters through SYS_EVENT_RETURN.
; -----------------------------------------------------------------------------

	.module event_common
	.include "hardware.inc"
	.include "memory.inc"

	.globl _kos_event_irq_exit
	.globl _kos_event_syscall_exit
	.globl _kos_event_post_bank
	.globl _kos_event_return_frame
	.globl _kos_event_pick
	.globl _kos_event_pick_keep
	.globl _kos_event_enter
	.globl _kos_event_unwind
	.globl _kos_event_table
	.globl _kos_event_in_handler
	.globl _kos_event_frame
	.globl _kos_event_frame_latch
	.globl _kos_event_row
	.globl _kos_event_latch
	.globl _kos_event_maybe
	.globl irq_saved_sp, irq_common_sp, irq_saved_latch
	.globl kos_gate_saved_sp, kos_gate_saved_latch, kos_gate_saved_iff
	.globl kos_gate_result_a, kos_gate_result_c, kos_gate_result_hl

; Table shape, byte for byte: kernel/event/event.h owns the C view and
; tests/test_events.py pins the two together.
EVENT_MAX   = 4                     ; KOS_EVENT_MAX
EVENT_ROW   = 12                    ; KOS_EVENT_ROW_SIZE
EVENT_SLOTS = 7                     ; KOS_EVENT_IN_HANDLER_SIZE
EVENT_OFF_BANK = 1                  ; KOS_EVENT_OFF_BANK
EVENT_OFF_PENDING = 2
EVENT_OFF_TRAMPOLINE = 6
EVENT_OFF_HANDLER = 8

EVENT_FRAME_SIZE = 24

	.area _COMMON

; ---------------------------------------------------------------------------
; uint16_t kos_event_pick(void);
; The C view of the scan below: DE = the row to deliver, 0 = none.  The
; cold half reaches it while the window is on bank 7, so it is the only
; entry point that may leave the C convention.
; Clobbers: AF, DE, HL, IX.
; ---------------------------------------------------------------------------
_kos_event_pick::
	push	bc
	push	ix
	push	iy
	call	_kos_event_pick_core
	pop	iy
	pop	ix
	pop	bc
	ex	de,hl
	ret

; ---------------------------------------------------------------------------
; void kos_event_pick_keep(void);
; The same scan for the two exit checks, which run with the caller's
; ABI-preserved registers (BC, DE, IX, IY) live and with no register to
; spare: everything is saved, and the result is read from kos_event_row.
; Clobbers: AF, HL.
; ---------------------------------------------------------------------------
_kos_event_pick_keep::
	push	bc
	push	de
	push	ix
	push	iy
	call	_kos_event_pick_core
	pop	iy
	pop	ix
	pop	de
	pop	bc
	ret

; The scan itself.  Every syscall and interrupt exit runs it, so it ends
; at once while kos_event_maybe is clear: a post sets it, and the scan
; clears it first and sets it again for any row still holding pending
; events (a post during the scan sets it too), so nothing is lost.
; Clobbers: AF, BC, DE, HL, IX.
_kos_event_pick_core::
	ld	a,(_kos_event_latch)
	and	#KOS_MODE_MASK
	cp	#KOS_MODE_APPLICATION
	jr	nz,event_pick_none
	ld	a,(_kos_event_maybe)
	or	a
	jr	z,event_pick_none
	xor	a
	ld	(_kos_event_maybe),a
	ld	a,(_kos_event_latch)
	and	#KOS_BANK_MASK
	ld	c,a
	ld	ix,#_kos_event_table
	ld	b,#EVENT_MAX
event_pick_row:
	ld	a,0(ix)			; owner: 0 marks a free row
	or	a
	jr	z,event_pick_next
	ld	a,EVENT_OFF_PENDING(ix)	; it needs pending events
	or	EVENT_OFF_PENDING+1(ix)
	jr	z,event_pick_next
	ld	a,#1			; some row still holds pending events
	ld	(_kos_event_maybe),a
	ld	a,EVENT_OFF_BANK(ix)	; the owner's bank must be the latch's
	cp	c
	jr	nz,event_pick_next
	ld	e,0(ix)			; and no handler of that process may
	ld	d,#0			; be running
	ld	hl,#_kos_event_in_handler
	add	hl,de
	ld	a,(hl)
	or	a
	jr	nz,event_pick_next
	push	ix
	pop	hl
	ld	(_kos_event_row),hl
	ret
event_pick_next:
	ld	de,#EVENT_ROW
	add	ix,de
	djnz	event_pick_row
event_pick_none:
	ld	hl,#0
	ld	(_kos_event_row),hl
	ret

; ---------------------------------------------------------------------------
; void kos_event_enter(void);  never returns.
;
; Entry: kos_event_row = the row to deliver; kos_event_latch = the latch of
; the interrupted code; kos_event_frame[owner] = the frame already built.
;
; Marks the handler active, snapshots and clears both counters, selects the
; application latch with the page field cleared (the trampoline is program
; image code, so it runs in the primary mapping), moves SP to the frame and
; RETIs to the trampoline with BC = handler, HL = count, DE = missed.  The
; trampoline's return address lands directly below the frame.
; Clobbers: everything (the frame is the application's state now).
; ---------------------------------------------------------------------------
_kos_event_enter::
	ld	ix,(_kos_event_row)
	ld	e,0(ix)
	ld	d,#0
	ld	hl,#_kos_event_in_handler
	add	hl,de
	ld	(hl),#1
	ld	hl,#_kos_event_frame_latch
	add	hl,de
	ld	a,(_kos_event_latch)	; the exact latch, for EVENT_RETURN
	ld	(hl),a
	ld	a,(_kos_event_latch)
	and	#(KOS_MODE_MASK | KOS_BANK_MASK)
	out	(KOS_MEMCTL_PORT),a
	sla	e			; the frame array is one word per process
	rl	d
	ld	hl,#_kos_event_frame
	add	hl,de
	ld	a,(hl)
	inc	hl
	ld	h,(hl)
	ld	l,a
	ld	sp,hl			; the frame memory is visible again
	ld	l,EVENT_OFF_TRAMPOLINE(ix)
	ld	h,EVENT_OFF_TRAMPOLINE+1(ix)
	push	hl			; the RETI target, below the frame
	ld	l,EVENT_OFF_PENDING(ix)
	ld	h,EVENT_OFF_PENDING+1(ix)
	ld	e,EVENT_OFF_PENDING+2(ix)
	ld	d,EVENT_OFF_PENDING+3(ix)
	xor	a
	ld	EVENT_OFF_PENDING(ix),a
	ld	EVENT_OFF_PENDING+1(ix),a
	ld	EVENT_OFF_PENDING+2(ix),a
	ld	EVENT_OFF_PENDING+3(ix),a
	ld	c,EVENT_OFF_HANDLER(ix)
	ld	b,EVENT_OFF_HANDLER+1(ix)
	ei
	reti

; ---------------------------------------------------------------------------
; void kos_event_unwind(uint16_t frame);  never returns.
;
; Entry: HL = the frame; kos_event_latch = the exact latch it was built
; under (mode, bank and page).  Restore that latch (the program stack is
; visible again), pop the frame in reverse and return to the interrupted PC
; with interrupts enabled.
; ---------------------------------------------------------------------------
_kos_event_unwind::
	ld	a,(_kos_event_latch)
	out	(KOS_MEMCTL_PORT),a
	ld	sp,hl
	pop	hl			; the (latch, 0) pair
	exx
	pop	hl			; HL'
	pop	de			; DE'
	pop	bc			; BC'
	exx
	ex	af,af'
	pop	af			; AF'
	ex	af,af'
	pop	iy
	pop	ix
	pop	hl
	pop	de
	pop	bc
	pop	af
	ei
	ret

; ---------------------------------------------------------------------------
; uint8_t kos_event_post_bank(uint8_t in_irq);
;
; The bank of the process the current context runs for.  Inside an interrupt
; that is the interrupted code's bank when it was application code (mode 10
; names one process, whether it runs in its own bank or through an aperture
; call); everything else - a driver body, or an interrupt that interrupted
; the nucleus - is serving the syscall in progress, whose caller latch the
; gate holds.  A posted event is pending exactly when that bank is the
; subscriber's.
; Clobbers: AF.
; ---------------------------------------------------------------------------
_kos_event_post_bank::
	or	a
	jr	z,event_post_bank_gate
	ld	a,(irq_saved_latch)
	and	#KOS_MODE_MASK
	cp	#KOS_MODE_APPLICATION
	jr	nz,event_post_bank_gate
	ld	a,(irq_saved_latch)
	jr	event_post_bank_have
event_post_bank_gate:
	ld	a,(kos_gate_saved_latch)
event_post_bank_have:
	and	#KOS_BANK_MASK
	ld	l,a
	ret

; ---------------------------------------------------------------------------
; uint16_t kos_event_return_frame(void);
; The frame the trampoline's RST 30h left below it: kos_gate_saved_sp is the
; SP after the RST pushed its return address, so the frame is two bytes up.
; ---------------------------------------------------------------------------
_kos_event_return_frame::
	ld	hl,(kos_gate_saved_sp)
	ld	de,#2
	add	hl,de
	ex	de,hl			; sdcccall(1): a 16-bit result is in DE
	ret

; ---------------------------------------------------------------------------
; void kos_event_irq_exit(void);  returns when nothing is deliverable.
;
; Called from kernel/irq/entry.asm after the interrupted latch is back and
; while the transition stack still holds the saved registers.  The frame is
; built below the interrupted SP: the stub's [AF][PC] pair at irq_saved_sp
; completes it, and the 18 saved register bytes move under it.
; ---------------------------------------------------------------------------
_kos_event_irq_exit::
	ld	hl,(irq_saved_sp)
	ld	de,#3
	add	hl,de
	ld	a,(hl)			; the interrupted PC's high byte
	cp	#>KOS_COMMON_BASE	; application code lives below common
	ret	nc
	ld	a,(irq_saved_latch)
	ld	(_kos_event_latch),a
	call	_kos_event_pick_keep
	ld	hl,(_kos_event_row)
	ld	a,h
	or	l
	ret	z
	; Build the frame below the interrupted stack.
	ld	hl,(irq_saved_sp)
	ld	de,#-(EVENT_FRAME_SIZE - 4)
	add	hl,de			; HL = irq_saved_sp - 20
	ld	a,(_kos_event_latch)
	ld	(hl),a			; +0: the latch
	inc	hl
	ld	(hl),#0			; +1
	inc	hl			; HL = the copy target, frame + 2
	ex	de,hl
	ld	hl,(irq_common_sp)	; the saved registers, HL' first
	ld	bc,#(EVENT_FRAME_SIZE - 6)
	ldir
	; Record the frame for its owner, then enter.
	ld	hl,(irq_saved_sp)
	ld	de,#-(EVENT_FRAME_SIZE - 4)
	add	hl,de
	push	hl
	ld	ix,(_kos_event_row)
	ld	e,0(ix)
	ld	d,#0
	sla	e
	rl	d
	ld	hl,#_kos_event_frame
	add	hl,de
	pop	de
	ld	(hl),e
	inc	hl
	ld	(hl),d
	jp	_kos_event_enter

; ---------------------------------------------------------------------------
; void kos_event_syscall_exit(void);  returns when nothing is deliverable.
;
; Called from kernel/syscall/entry.asm after the caller's latch and SP are
; restored and before the result registers are rebuilt.  The frame is built
; below the caller's RST return address and carries the syscall's result
; registers, so the handler returns into the instruction after the syscall
; with the result intact.
; ---------------------------------------------------------------------------
_kos_event_syscall_exit::
	ld	a,(kos_gate_saved_iff)
	or	a
	ret	z			; the caller had interrupts masked
	ld	hl,(kos_gate_saved_sp)
	inc	hl
	ld	a,(hl)			; the return PC's high byte
	cp	#>KOS_COMMON_BASE
	ret	nc
	ld	a,(kos_gate_saved_latch)
	ld	(_kos_event_latch),a
	call	_kos_event_pick_keep
	ld	hl,(_kos_event_row)
	ld	a,h
	or	l
	ret	z
	; Build the frame by pushing downward from the caller's SP; the RST
	; return address is already at the top of it (frame + 22).  This call's
	; own return address goes first: the frame starts 22 bytes below the
	; caller's SP, so that the RST return address is exactly frame + 22 and
	; the unwind leaves SP at the caller's SP (as the IRQ path does).
	pop	hl
	ld	a,(kos_gate_result_c)
	rra				; the result flag -> carry
	ld	a,(kos_gate_result_a)
	push	af			; +20: AF, as the syscall returns it
	push	bc			; +18
	push	de			; +16
	ld	hl,(kos_gate_result_hl)
	push	hl			; +14: the primary result
	push	ix			; +12
	push	iy			; +10
	ex	af,af'
	push	af			; +8: AF', unspecified across a syscall
	ex	af,af'
	exx
	push	bc			; +6
	push	de			; +4
	push	hl			; +2
	exx
	ld	a,(_kos_event_latch)
	ld	l,a
	ld	h,#0
	push	hl			; +0: the latch byte, then 0
	; Record the frame for its owner: SP is the frame.
	ld	hl,#0
	add	hl,sp
	push	hl
	ld	ix,(_kos_event_row)
	ld	e,0(ix)
	ld	d,#0
	sla	e
	rl	d
	ld	hl,#_kos_event_frame
	add	hl,de
	pop	de
	ld	(hl),e
	inc	hl
	ld	(hl),d
	jp	_kos_event_enter

; ---------------------------------------------------------------------------
; State, all of it common memory: the table and the delivery state are read
; while the application's own latch is in force (the delivery) and while the
; driver's window is mapped (event_post).
; ---------------------------------------------------------------------------
	.area _COMMON_DATA
_kos_event_table::
	.ds	EVENT_MAX * EVENT_ROW
_kos_event_in_handler::
	.ds	EVENT_SLOTS
_kos_event_frame::		; one word per process slot
	.ds	EVENT_SLOTS * 2
_kos_event_frame_latch::
	.ds	EVENT_SLOTS
_kos_event_row::
	.ds	2
_kos_event_latch::
	.ds	1
_kos_event_maybe::		; 1 while some row may hold pending events
	.ds	1
