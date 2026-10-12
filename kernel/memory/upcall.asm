; -----------------------------------------------------------------------------
; KonaOS revision-13 upcall shim (architecture 36.5).
;
; The fixed primitive that lets cold-half nucleus code call a resident
; driver: it validates the request, saves the exact banking latch,
; selects the window page (P), makes one call into the window
; 6000h-DFFFh, restores the saved latch, and publishes the target's HL
; result and its A register at return.
;
;   struct kos_upcall_req {
;       uint8_t  page;   0 = bank 7's cold half, 1 = bank 6
;       uint16_t entry;  target address, must be in 6000h-DFFFh
;       void    *args;   target argument, passed in HL
;       uint16_t result; target result, read from HL
;   };
;   uint8_t kos_upcall(const struct kos_upcall_req *req);
;
; SDCC sdcccall(1): the single 16-bit pointer argument arrives in HL; the
; 8-bit result is returned in A (0, or KOS_EINVAL).
;
; Resident in the fixed core: the decoder answers 6000h-DFFFh from
; bank 7 - P, so the shim's own code, its continuation after the latch
; write and the request block must stay below 6000h.  Nothing here changes
; the mode or the selected bank, so no common-resident continuation is
; needed.
;
; The target runs on the caller's stack, which therefore has to stay
; visible whatever the window shows (the common region or the fixed core).
; Maskable interrupts may arrive during the call: the IRQ gate saves the
; interrupted latch in its own slot and dispatches with P = 0, so the
; upcall resumes with its page still selected.
; -----------------------------------------------------------------------------

	.module upcall
	.include "hardware.inc"
	.include "memory.inc"

	.globl _kos_upcall
	.globl kos_upcall_end
	.globl kos_upcall_page
	.globl kos_upcall_entry
	.globl kos_upcall_args
	.globl kos_upcall_result
	.globl kos_upcall_result_ptr
	.globl _kos_upcall_ret_a
	.globl kos_upcall_saved_latch

; Window pages the operating system uses (mirror kernel/memory/upcall.h).
KOS_UPCALL_PAGE_NUCLEUS = 0
KOS_UPCALL_PAGE_DRIVER = 1

	.area _CODE

; ---------------------------------------------------------------------------
; _kos_upcall - C name kos_upcall.  Entry: HL = request pointer.
;
; Decode and validate everything while bank 7 is still fully visible (the
; caller runs with P = 0), then switch the page, call, and restore.
; Clobbers: AF, BC, DE, HL (the target call clobbers everything).
; ---------------------------------------------------------------------------
_kos_upcall::
	ld	a,(hl)			; page
	cp	#(KOS_UPCALL_PAGE_DRIVER + 1)
	jr	c,001$
	ld	a,#0x02			; KOS_EINVAL
	ret
001$:
	ld	(kos_upcall_page),a
	inc	hl
	ld	a,(hl)			; entry low
	ld	(kos_upcall_entry),a
	inc	hl
	ld	a,(hl)			; entry high
	cp	#(KOS_REV13_WINDOW_BASE >> 8)
	jr	nc,002$
	ld	a,#0x02			; KOS_EINVAL: not in the window
	ret
002$:
	ld	(kos_upcall_entry+1),a
	inc	hl
	ld	a,(hl)			; args low
	ld	(kos_upcall_args),a
	inc	hl
	ld	a,(hl)			; args high
	ld	(kos_upcall_args+1),a
	inc	hl
	ld	(kos_upcall_result_ptr),hl	; &req->result, for the write-back

	; Save the exact latch, then select the page: only D7..D5 change, so
	; the caller's mode and bank survive the call.
	in	a,(KOS_MEMCTL_PORT)
	ld	(kos_upcall_saved_latch),a
	and	#(KOS_MODE_MASK | KOS_BANK_MASK)
	ld	c,a
	ld	a,(kos_upcall_page)
	rrca
	rrca
	rrca
	and	#KOS_ROM_PAGE_MASK
	or	c
	out	(KOS_MEMCTL_PORT),a

	; One call into the window.  Z80 has no indirect call: push the
	; return address, push the target, and let the target's RET return
	; into the continuation below.  Both pushes go to the caller's
	; stack, which stays visible across the page change.
	ld	bc,#kos_upcall_return
	push	bc
	ld	bc,(kos_upcall_entry)
	push	bc
	ld	hl,(kos_upcall_args)
	ret
kos_upcall_return:
	; Back to the caller's exact latch before touching its memory; the
	; request block is in the fixed core, visible either way.  A holds
	; the target's 8-bit return, so it is published before this code
	; clobbers it.
	ld	(_kos_upcall_ret_a),a
	ld	a,(kos_upcall_saved_latch)
	out	(KOS_MEMCTL_PORT),a
	ld	(kos_upcall_result),hl
	ld	hl,(kos_upcall_result_ptr)
	ld	a,(kos_upcall_result)
	ld	(hl),a
	inc	hl
	ld	a,(kos_upcall_result+1)
	ld	(hl),a
	xor	a
	ret
kos_upcall_end::

	.area _DATA

; Single-depth transition state (not reentrant; see the module header).
kos_upcall_page::
	.ds	1
kos_upcall_entry::
	.ds	2
kos_upcall_args::
	.ds	2
kos_upcall_result::
	.ds	2
kos_upcall_result_ptr::
	.ds	2
_kos_upcall_ret_a::
	.ds	1
kos_upcall_saved_latch::
	.ds	1
