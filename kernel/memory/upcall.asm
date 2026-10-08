; -----------------------------------------------------------------------------
; ZephyrOS revision-13 upcall shim (architecture 36.5).
;
; The fixed primitive that lets cold-half nucleus code call a resident
; service: it validates the request, saves the exact banking latch,
; selects the window page (P), makes one call into the window
; 6000h-DFFFh, restores the saved latch, and publishes the target's HL
; result.
;
;   struct zos_upcall_req {
;       uint8_t  page;   0 = bank 7's cold half, 1 = bank 6
;       uint16_t entry;  target address, must be in 6000h-DFFFh
;       void    *args;   target argument, passed in HL
;       uint16_t result; target result, read from HL
;   };
;   uint8_t zos_upcall(const struct zos_upcall_req *req);
;
; SDCC sdcccall(1): the single 16-bit pointer argument arrives in HL; the
; 8-bit result is returned in A (0, or ZOS_EINVAL).
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
	.include "zephyr80/hardware.inc"
	.include "zephyr80/memory.inc"

	.globl _zos_upcall
	.globl zos_upcall_end
	.globl zos_upcall_page
	.globl zos_upcall_entry
	.globl zos_upcall_args
	.globl zos_upcall_result
	.globl zos_upcall_result_ptr
	.globl zos_upcall_saved_latch

; Window pages the operating system uses (mirror kernel/memory/upcall.h).
ZOS_UPCALL_PAGE_NUCLEUS = 0
ZOS_UPCALL_PAGE_SERVICE = 1

	.area _CODE

; ---------------------------------------------------------------------------
; _zos_upcall - C name zos_upcall.  Entry: HL = request pointer.
;
; Decode and validate everything while bank 7 is still fully visible (the
; caller runs with P = 0), then switch the page, call, and restore.
; Clobbers: AF, BC, DE, HL (the target call clobbers everything).
; ---------------------------------------------------------------------------
_zos_upcall::
	ld	a,(hl)			; page
	cp	#(ZOS_UPCALL_PAGE_SERVICE + 1)
	jr	c,001$
	ld	a,#0x02			; ZOS_EINVAL
	ret
001$:
	ld	(zos_upcall_page),a
	inc	hl
	ld	a,(hl)			; entry low
	ld	(zos_upcall_entry),a
	inc	hl
	ld	a,(hl)			; entry high
	cp	#(ZOS_REV13_WINDOW_BASE >> 8)
	jr	nc,002$
	ld	a,#0x02			; ZOS_EINVAL: not in the window
	ret
002$:
	ld	(zos_upcall_entry+1),a
	inc	hl
	ld	a,(hl)			; args low
	ld	(zos_upcall_args),a
	inc	hl
	ld	a,(hl)			; args high
	ld	(zos_upcall_args+1),a
	inc	hl
	ld	(zos_upcall_result_ptr),hl	; &req->result, for the write-back

	; Save the exact latch, then select the page: only D7..D5 change, so
	; the caller's mode and bank survive the call.
	in	a,(ZOS_MEMCTL_PORT)
	ld	(zos_upcall_saved_latch),a
	and	#(ZOS_MODE_MASK | ZOS_BANK_MASK)
	ld	c,a
	ld	a,(zos_upcall_page)
	rrca
	rrca
	rrca
	and	#ZOS_ROM_PAGE_MASK
	or	c
	out	(ZOS_MEMCTL_PORT),a

	; One call into the window.  Z80 has no indirect call: push the
	; return address, push the target, and let the target's RET return
	; into the continuation below.  Both pushes go to the caller's
	; stack, which stays visible across the page change.
	ld	bc,#zos_upcall_return
	push	bc
	ld	bc,(zos_upcall_entry)
	push	bc
	ld	hl,(zos_upcall_args)
	ret
zos_upcall_return:
	; Back to the caller's exact latch before touching its memory; the
	; request block is in the fixed core, visible either way.
	ld	a,(zos_upcall_saved_latch)
	out	(ZOS_MEMCTL_PORT),a
	ld	(zos_upcall_result),hl
	ld	hl,(zos_upcall_result_ptr)
	ld	a,(zos_upcall_result)
	ld	(hl),a
	inc	hl
	ld	a,(zos_upcall_result+1)
	ld	(hl),a
	xor	a
	ret
zos_upcall_end::

	.area _DATA

; Single-depth transition state (not reentrant; see the module header).
zos_upcall_page::
	.ds	1
zos_upcall_entry::
	.ds	2
zos_upcall_args::
	.ds	2
zos_upcall_result::
	.ds	2
zos_upcall_result_ptr::
	.ds	2
zos_upcall_saved_latch::
	.ds	1
