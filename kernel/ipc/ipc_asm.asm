; -----------------------------------------------------------------------------
; KonaOS fixed-core IPC primitives (kernel/ipc, architecture 4.9).
;
; Every symbol here is fixed-core by construction: it must stay visible in
; 2000h-5FFFh while the resident driver domain (bank 6) is mapped into
; the mode-11 window 6000h-DFFFh.  Nothing here touches cold code or cold
; data while a domain is open.
;
; The primitives are deliberately dumb: save/select/restore the mapping, a
; plain LDIR copy, the driver-stack switch around one CALL, and the CALL
; itself.  All validation, tables, frames and descriptors live in the
; nucleus C object (kernel/ipc/ipc.c); this file must not grow policy.
;
; Calling conventions are SDCC sdcccall(1), the kernel's own:
;   16-bit return in DE, 8-bit return in A, first 16-bit argument in HL,
;   second 16-bit argument in DE, first 8-bit argument in A.
;
; The driver stack is the 512-byte reservation of architecture 4.9 at
; KOS_DRIVER_STACK_BASE, below the relocated ISR stack; like the ISR
; stack it is declared ABS so no ordinary allocation can enter it.
; -----------------------------------------------------------------------------

	.module ipc_asm
	.include "hardware.inc"
	.include "memory.inc"

	.globl _ipc_latch
	.globl _ipc_gate_latch
	.globl _ipc_domain_enter, _ipc_domain_set, _ipc_domain_exit
	.globl _ipc_copy_req, _ipc_copy_run
	.globl _ipc_invoke, _ipc_invoke_here
	.globl _ipc_saved_latch
	.globl kos_gate_saved_latch
	.globl kos_driver_stack, kos_driver_stack_top
	.globl ipc_asm_begin, ipc_asm_end
	.globl ipc_domain_switch_end

	.area _CODE

ipc_asm_begin::

; ---------------------------------------------------------------------------
; uint8_t ipc_latch(void);            read the banking latch (A)
; ---------------------------------------------------------------------------
_ipc_latch::
	in	a,(KOS_MEMCTL_PORT)
	ret

; ---------------------------------------------------------------------------
; uint8_t ipc_gate_latch(void);
;
; The syscall gate's saved caller latch (kernel/syscall/entry.asm).  The
; C core cannot name an assembly global without the C underscore prefix,
; so the read goes through this two-instruction accessor.
; ---------------------------------------------------------------------------
_ipc_gate_latch::
	ld	a,(kos_gate_saved_latch)
	ret

; ---------------------------------------------------------------------------
; void ipc_domain_enter(uint8_t page_bits);   A = page bits (revision 13)
;
; Saves the full current latch in fixed state and writes
; (latch & bank field) | mode 11 | page_bits.  The bank field is
; preserved, so the caller's low window (0000h-1FFFh) keeps showing the
; caller's bank while the driver domain is mapped.  Register-only: safe
; to call with any visible stack.
; ---------------------------------------------------------------------------
_ipc_domain_enter::
	ld	b,a			; B = page bits
	in	a,(KOS_MEMCTL_PORT)
	ld	(_ipc_saved_latch),a
	and	#KOS_BANK_MASK
	or	#KOS_MODE_OS
	or	b
	out	(KOS_MEMCTL_PORT),a
	ret

; ---------------------------------------------------------------------------
; void ipc_domain_set(uint8_t page_bits);     A = page bits
;
; Same mapping change as ipc_domain_enter but without touching the saved
; latch cell: the IRQ trampoline and a cross-domain nested call re-select
; a window page inside a call whose own enter/exit pair must stay intact
; (architecture 4.5: the IRQ entry owns its own latch slot).
; ---------------------------------------------------------------------------
_ipc_domain_set::
	ld	b,a			; B = page bits
	in	a,(KOS_MEMCTL_PORT)
	and	#KOS_BANK_MASK
	or	#KOS_MODE_OS
	or	b
	out	(KOS_MEMCTL_PORT),a
	ret

; ---------------------------------------------------------------------------
; void ipc_domain_exit(void);         restore the saved latch exactly
; ---------------------------------------------------------------------------
_ipc_domain_exit::
	ld	a,(_ipc_saved_latch)
	out	(KOS_MEMCTL_PORT),a
	ret

; The three page selects above are the fixed core's only mapping changes:
; each one writes mode 11 with new window page bits, which leaves the fixed
; core (2000h-5FFFh) itself mapped - the instruction after the OUT is
; fetched from the same place.  The extent is exported so the layout check
; can allow exactly these writes (the mode-10 aperture change lives in the
; common-resident ipc_common.asm).
ipc_domain_switch_end::

; ---------------------------------------------------------------------------
; uint16_t ipc_copy_run(void);        run the LDIR described by
;                                     ipc_copy_req {src, dst, len}
;
; Returns the length in DE.  The request block is fixed memory and the
; primitive is not re-entrant: the nucleus serialises copies.  Runs while
; the window shows the driver domain; both addresses must be visible in
; that mapping (the copy resolvers in ipc.c guarantee it).
; ---------------------------------------------------------------------------
_ipc_copy_run::
	ld	hl,#_ipc_copy_req
	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	inc	hl
	ld	(_ipc_src),de
	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	inc	hl
	ld	(_ipc_dst),de
	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	ld	(_ipc_len),de
	ld	hl,(_ipc_src)
	ld	de,(_ipc_dst)
	ld	bc,(_ipc_len)
	ldir
	ld	de,(_ipc_len)
	ret

; ---------------------------------------------------------------------------
; int16_t ipc_invoke(uint16_t entry, struct kos_ipc_frame *frame);
;   HL = entry (a window address in the resident domain),
;   DE = frame (nucleus-owned copy).
;
; The outermost call: saves the nucleus stack, switches to the fixed
; driver stack, CALLs the driver dispatch entry with HL = frame,
; restores the nucleus stack and returns the driver's result in DE.
; The domain must already be open.
;
; The two entry points keep separate return paths on purpose: an outermost
; call can be interrupted by any number of nested calls, so a shared
; "which mode am I in" cell would let the inner call decide the outer
; call's epilogue (and strand the nucleus on the driver stack once that
; stack runs out).
; ---------------------------------------------------------------------------
_ipc_invoke::
	ld	(_ipc_saved_sp),sp
	ld	sp,#kos_driver_stack_top
	ex	de,hl			; DE = entry, HL = frame
	ld	(_ipc_entry),de
	ld	de,#001$
	push	de			; return address for the dispatch entry
	ld	de,(_ipc_entry)
	push	de
	ret				; enter the driver dispatch
001$:
	ld	(_ipc_result),de
	ld	sp,(_ipc_saved_sp)
	ld	de,(_ipc_result)
	ret

; ---------------------------------------------------------------------------
; int16_t ipc_invoke_here(uint16_t entry, struct kos_ipc_frame *frame);
;
; A nested call: the driver stack is already active, so only the CALL
; happens.  Same result convention.  This is the same-domain fast path:
; the window already shows the target's bank, so the mapping is untouched.
; ---------------------------------------------------------------------------
_ipc_invoke_here::
	ex	de,hl			; DE = entry, HL = frame
	ld	(_ipc_entry),de
	ld	de,#002$
	push	de			; return address for the dispatch entry
	ld	de,(_ipc_entry)
	push	de
	ret				; enter the driver dispatch
002$:
	ld	(_ipc_result),de
	ld	de,(_ipc_result)
	ret

ipc_asm_end::

; ---------------------------------------------------------------------------
; Fixed state.  _DATA is the fixed runtime data area, so none of it
; disappears while a domain is open.
; ---------------------------------------------------------------------------
	.area _DATA
_ipc_saved_latch::
	.ds	1
_ipc_entry::
	.ds	2
_ipc_saved_sp::
	.ds	2
_ipc_result::
	.ds	2
_ipc_src::
	.ds	2
_ipc_dst::
	.ds	2
_ipc_len::
	.ds	2
_ipc_copy_req::
	.ds	6

; ---------------------------------------------------------------------------
; The driver-domain stack (architecture 4.9): 512 bytes in the fixed
; core, shared by the whole driver-call chain.  ABS so no ordinary
; allocation can enter the reservation.
; ---------------------------------------------------------------------------
	.area _IPC_DRIVER_STACK (ABS)
	.org KOS_DRIVER_STACK_BASE
kos_driver_stack::
	.ds	KOS_DRIVER_STACK_SIZE
kos_driver_stack_top::
