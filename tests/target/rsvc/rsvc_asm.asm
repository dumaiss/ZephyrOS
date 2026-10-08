; -----------------------------------------------------------------------------
; ZephyrOS resident-service spike: fixed-region domain primitives (test only).
;
; Linked only into an evaluation image (ZOS_RSVC_SPIKE=1).  Every symbol
; here is fixed-core by construction: it must stay visible in 2000h-7FFFh
; while the resident service domain (bank 6) is mapped into the mode-11
; window or slot.  Nothing here touches cold code while a domain is open.
;
; The primitives are deliberately dumb: save/enter/leave the mapping, a
; plain LDIR copy, the service-stack switch around one CALL, and the CALL
; itself.  All validation, tables, frames and descriptors live in the
; nucleus C object (tests/target/rsvc/rsvc_core.c); this file must not
; grow policy.
;
; Calling conventions are SDCC sdcccall(1), the kernel's own:
;   16-bit return in DE, 8-bit return in A, first 16-bit argument in HL,
;   second 16-bit argument in DE, first 8-bit argument in A.
; -----------------------------------------------------------------------------

	.module rsvc_asm
	.include "zephyr80/hardware.inc"
	.include "zephyr80/memory.inc"

	.globl _rsvc_latch, _rsvc_latch_saved
	.globl _rsvc_domain_enter, _rsvc_domain_exit, _rsvc_domain_set
	.globl _rsvc_copy_req, _rsvc_copy_run
	.globl _rsvc_invoke, _rsvc_invoke_here
	.globl _rsvc_saved_sp, _rsvc_service_stack, _rsvc_service_stack_top
	.globl _rsvc_gate_stack_top, _rsvc_isr_stack, _rsvc_isr_stack_size
	; Kernel-owned symbols this file only reads.
	.globl zos_gate_stack_top, irq_isr_stack

	; -------------------------------------------------------------------
	; uint8_t rsvc_latch(void);          read the banking latch (A)
	; uint8_t rsvc_latch_saved(void);    the latch saved by domain_enter
	; -------------------------------------------------------------------
	.area _CODE
_rsvc_latch::
	in	a,(ZOS_MEMCTL_PORT)
	ret

_rsvc_latch_saved::
	ld	a,(_rsvc_saved_latch)
	ret

	; -------------------------------------------------------------------
	; void rsvc_domain_enter(uint8_t page_bits);   A = page bits
	;
	; Saves the full current latch in fixed state and writes
	; (latch & bank field) | mode 11 | page_bits.  The bank field is
	; preserved, so the caller's low window (0000h-1FFFh) keeps showing
	; the caller's bank while the domain is mapped.
	; -------------------------------------------------------------------
_rsvc_domain_enter::
	ld	b,a			; B = page bits
	in	a,(ZOS_MEMCTL_PORT)
	ld	(_rsvc_saved_latch),a
	and	#ZOS_BANK_MASK
	or	#ZOS_MODE_OS
	or	b
	out	(ZOS_MEMCTL_PORT),a
	ret

	; -------------------------------------------------------------------
	; void rsvc_domain_set(uint8_t page_bits);    A = page bits
	;
	; Same mapping change as rsvc_domain_enter but without touching the
	; saved-latch cell: the transport-B cross-slot relay switches slots
	; inside a call whose own enter/exit pair must stay intact.
	; -------------------------------------------------------------------
_rsvc_domain_set::
	ld	b,a			; B = page bits
	in	a,(ZOS_MEMCTL_PORT)
	and	#ZOS_BANK_MASK
	or	#ZOS_MODE_OS
	or	b
	out	(ZOS_MEMCTL_PORT),a
	ret

	; -------------------------------------------------------------------
	; void rsvc_domain_exit(void);       restore the saved latch exactly
	; -------------------------------------------------------------------
_rsvc_domain_exit::
	ld	a,(_rsvc_saved_latch)
	out	(ZOS_MEMCTL_PORT),a
	ret

	; -------------------------------------------------------------------
	; uint16_t rsvc_copy_run(void);      run the LDIR described by
	;                                    rsvc_copy_req {src, dst, len}
	;
	; Returns the length in DE.  The request block is fixed memory and
	; the primitive is not re-entrant: the nucleus serialises copies.
	; -------------------------------------------------------------------
_rsvc_copy_run::
	ld	hl,#_rsvc_copy_req
	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	inc	hl
	ld	(_rsvc_src),de
	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	inc	hl
	ld	(_rsvc_dst),de
	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	ld	(_rsvc_len),de
	ld	hl,(_rsvc_src)
	ld	de,(_rsvc_dst)
	ld	bc,(_rsvc_len)
	ldir
	ld	de,(_rsvc_len)
	ret

	; -------------------------------------------------------------------
	; int16_t rsvc_invoke(uint16_t entry, struct rsvc_frame *frame);
	;   HL = entry (a window address in the resident domain),
	;   DE = frame (nucleus-owned copy).
	;
	; The outermost call: saves the nucleus stack, switches to the common
	; service stack, CALLs the service dispatch entry with HL = frame,
	; restores the nucleus stack and returns the service's result in DE.
	; The domain must already be open.
	;
	; The two entry points keep separate return paths on purpose: an
	; outermost call can be interrupted by any number of nested calls, so
	; a shared "which mode am I in" cell would let the inner call decide
	; the outer call's epilogue (and strand the nucleus on the service
	; stack once that stack runs out).
	; -------------------------------------------------------------------
_rsvc_invoke::
	ld	(_rsvc_saved_sp),sp
	ld	sp,#_rsvc_service_stack_top
	ex	de,hl			; DE = entry, HL = frame
	ld	(_rsvc_entry),de
	ld	de,#001$
	push	de			; return address for the dispatch entry
	ld	de,(_rsvc_entry)
	push	de
	ret				; enter the service dispatch
001$:
	ld	(_rsvc_result),de
	ld	sp,(_rsvc_saved_sp)
	ld	de,(_rsvc_result)
	ret

	; -------------------------------------------------------------------
	; int16_t rsvc_invoke_here(uint16_t entry, struct rsvc_frame *frame);
	;
	; A nested call: the service stack is already active, so only the
	; CALL happens.  Same result convention.
	; -------------------------------------------------------------------
_rsvc_invoke_here::
	ex	de,hl			; DE = entry, HL = frame
	ld	(_rsvc_entry),de
	ld	de,#002$
	push	de			; return address for the dispatch entry
	ld	de,(_rsvc_entry)
	push	de
	ret				; enter the service dispatch
002$:
	ld	(_rsvc_result),de
	ld	de,(_rsvc_result)
	ret

	; -------------------------------------------------------------------
	; C-visible access to the kernel stacks.  The kernel defines these
	; symbols in its own (underscore-free) assembler namespace, so the
	; C object reaches them through these getters.
	; -------------------------------------------------------------------
_rsvc_gate_stack_top::
	ld	hl,#zos_gate_stack_top
	ex	de,hl
	ret

_rsvc_isr_stack::
	ld	hl,#irq_isr_stack
	ex	de,hl
	ret

_rsvc_isr_stack_size::
	ld	de,#ZOS_IRQ_ISR_STACK_SIZE
	ret

	; -------------------------------------------------------------------
	; Fixed state.  _DATA is the fixed runtime data area in the
	; evaluation layout (cold objects use _WINDOW_DATA), so none of it
	; disappears while a domain is open.
	; -------------------------------------------------------------------
	.area _DATA
_rsvc_saved_latch::
	.ds	1
_rsvc_entry::
	.ds	2
_rsvc_saved_sp::
	.ds	2
_rsvc_result::
	.ds	2
_rsvc_src::
	.ds	2
_rsvc_dst::
	.ds	2
_rsvc_len::
	.ds	2
_rsvc_copy_req::
	.ds	6

	; -------------------------------------------------------------------
	; The service-domain stack lives in common memory: it is mapped
	; whatever window page or slot is open, and an interrupt that
	; arrives while a service runs switches to the nucleus ISR stack
	; without needing this stack to be visible.  ZOS_RSVC_SERVICE_STACK_SIZE
	; bytes, grown down; the size must keep the common ceiling (see
	; tests/target/rsvc/rsvc_config.h).
	; -------------------------------------------------------------------
	.area _COMMON_DATA
	.include "rsvc_stack_size.inc"

_rsvc_service_stack::
	.ds	ZOS_RSVC_SERVICE_STACK_SIZE
_rsvc_service_stack_top::
	.ds	0
