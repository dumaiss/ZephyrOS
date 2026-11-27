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
; Assembled for Mk2 (kernel/memory/mk2/map_ops.inc defines KOS_MAP_DAT)
; only the driver-stack calls, the IRQ trampoline and the stack remain:
; the Mk2 driver domain is mapped through the span routines (dat_map.asm),
; and descriptor copies run through the page copy.
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
	.include "map_ops.inc"
	.include "kernel/runtime/fastcopy.inc"

	.globl _ipc_invoke, _ipc_invoke_here
	.globl kos_driver_stack, kos_driver_stack_top
	.globl ipc_asm_begin, ipc_asm_end
	.ifdef KOS_MAP_DAT
	.globl _ipc_irq_trampoline
	.globl _irq_handler_entry, _irq_handler_context, _ipc_irq_active
	.globl _kos_driver_domain
	.endif
	.ifndef KOS_MAP_DAT
	.globl _ipc_latch
	.globl _ipc_gate_latch
	.globl _ipc_domain_enter, _ipc_domain_set, _ipc_domain_exit
	.globl _ipc_copy_req, _ipc_copy_run
	.globl _ipc_saved_latch
	.globl _ipc_bulk_map, _ipc_bulk_unmap
	.globl _ipc_bulk_open, _ipc_bulk_faults
	.globl kos_gate_saved_latch
	.globl ipc_domain_switch_end
	.endif

; Bank 6, the resident driver domain (architecture 2.3), and the busy
; answer of a nested span (abi/kos/errno.h KOS_EBUSY).
IPC_ASM_DRIVER_BANK = 6
IPC_ASM_EBUSY       = 4

	.area _CODE

ipc_asm_begin::

	.ifndef KOS_MAP_DAT
; ---------------------------------------------------------------------------
; uint8_t ipc_latch(void);            read the banking latch (A)
; ---------------------------------------------------------------------------
_ipc_latch::
	KOS_MAP_CURRENT
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
	KOS_MAP_CURRENT
	ld	(_ipc_saved_latch),a
	KOS_MAP_BANK_OF
	KOS_MAP_SYSTEM_WINDOW
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
	KOS_MAP_CURRENT
	KOS_MAP_BANK_OF
	KOS_MAP_SYSTEM_WINDOW
	ret

; ---------------------------------------------------------------------------
; void ipc_domain_exit(void);         restore the saved latch exactly
; ---------------------------------------------------------------------------
_ipc_domain_exit::
	ld	a,(_ipc_saved_latch)
	KOS_MAP_RESTORE
	ret

; ---------------------------------------------------------------------------
; int16_t ipc_bulk_map(void);         open a bulk-map span (architecture 5.2)
;
; Writes Q = 6, keeping the mode and the page: 0000h-1FFFh then shows bank
; 6's first 8 KiB, where the planner put the drivers' mapped segments.
; The span state is one byte: 0 when closed, 80h | the replaced Q when
; open.  Returns 0 in DE, or -KOS_EBUSY with nothing changed when a span
; is already open (spans do not nest).  The state is recorded before the
; OUT, so an interrupt between the two finds the span already open - and
; its exact-latch restore returns to Q = 6.
; ---------------------------------------------------------------------------
_ipc_bulk_map::
	ld	a,(_ipc_bulk_open)
	or	a
	jr	nz,010$
	KOS_MAP_CURRENT
	ld	b,a
	KOS_MAP_BANK_OF
	or	#0x80
	ld	(_ipc_bulk_open),a
	ld	a,b
	KOS_MAP_CALLER_WINDOW #IPC_ASM_DRIVER_BANK
	ld	de,#0
	ret
010$:
	ld	de,#-IPC_ASM_EBUSY
	ret

; ---------------------------------------------------------------------------
; void ipc_bulk_unmap(void);          close the span: restore the saved Q
;
; Outside a span it changes nothing.
; ---------------------------------------------------------------------------
_ipc_bulk_unmap::
	ld	a,(_ipc_bulk_open)
	or	a
	ret	z
	and	#KOS_BANK_MASK
	ld	b,a
	KOS_MAP_CURRENT
	KOS_MAP_CALLER_WINDOW b
	xor	a
	ld	(_ipc_bulk_open),a
	ret

; The page selects and the span above are the fixed core's only mapping
; changes: each one writes mode 11 with new window page bits or a new bank
; field, which leaves the fixed core (2000h-5FFFh) itself mapped - the
; instruction after the OUT is fetched from the same place.  The extent is exported so the layout check
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
	KOS_FASTCOPY
	ld	de,(_ipc_len)
	ret
	.endif

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
	call	ipc_enter		; the dispatch returns here, DE = result
	ld	sp,(_ipc_saved_sp)
	ret

; ---------------------------------------------------------------------------
; int16_t ipc_invoke_here(uint16_t entry, struct kos_ipc_frame *frame);
;
; A nested call: the driver stack is already active, so only the CALL
; happens: the dispatch entry returns straight to the caller.  Same result
; convention.  This is the same-domain fast path: the window already shows
; the target's bank, so the mapping is untouched.
; ---------------------------------------------------------------------------
_ipc_invoke_here::
ipc_enter:
	ex	de,hl			; DE = entry, HL = frame
	push	de
	ret				; enter the driver dispatch

	.ifdef KOS_MAP_DAT
; ---------------------------------------------------------------------------
; void ipc_irq_trampoline(void *context);     HL = the IRQ source
;
; The nucleus's handler for every driver-owned source, called by the IRQ
; dispatcher on the ISR stack with interrupts masked: the driver's handler
; for the source runs with the domain at system entries 3-6, then exactly
; what the interrupted code had there goes back (a cold set, a copy window,
; the domain itself).  The mirror is what the interrupted code has
; installed or is installing (an interrupted remap writes it before its
; OUT), so the hardware may still show the previous page in one entry: the
; domain is written whatever the mirror says, and the saved mirrors go
; back.  Each entry is written mirror first, in ascending order, as the
; span routines (dat_map.asm) do; the whole path is straight-line so that
; a tick handler fits the SIO receive FIFO's three bytes at 115,200 baud.
; Registers: IX is kept; everything else is the handler's.
; ---------------------------------------------------------------------------
_ipc_irq_trampoline::
	ld	a,l			; the source, validated by api_irq_claim
	add	a,a
	ld	c,a
	ld	b,#0			; BC = source * 2
	ld	hl,#_irq_handler_entry
	add	hl,bc
	ld	e,(hl)
	inc	hl
	ld	d,(hl)			; DE = the handler
	ld	a,d
	or	e
	ret	z			; no handler claimed
	ld	hl,#_irq_handler_context
	add	hl,bc
	ld	c,(hl)
	inc	hl
	ld	b,(hl)			; BC = its context
	ld	a,#1
	ld	(_ipc_irq_active),a
	ld	hl,(KOS_DAT_SYSTEM + 5)
	push	hl			; entries 5, 6
	ld	hl,(KOS_DAT_SYSTEM + 3)
	push	hl			; entries 3, 4
	ld	hl,(_kos_driver_domain + 3)
	ld	a,l
	KOS_DAT_SET 3
	ld	a,h
	KOS_DAT_SET 4
	ld	hl,(_kos_driver_domain + 5)
	ld	a,l
	KOS_DAT_SET 5
	ld	a,h
	KOS_DAT_SET 6
	ld	h,b
	ld	l,c			; HL = the context
	call	ipc_irq_enter
	pop	hl
	ld	a,l
	KOS_DAT_SET 3
	ld	a,h
	KOS_DAT_SET 4
	pop	hl
	ld	a,l
	KOS_DAT_SET 5
	ld	a,h
	KOS_DAT_SET 6
	xor	a
	ld	(_ipc_irq_active),a
	ret
ipc_irq_enter:
	push	de			; the handler's entry returns to the caller
	ret
	.endif

ipc_asm_end::

; ---------------------------------------------------------------------------
; Fixed state.  _DATA is the fixed runtime data area, so none of it
; disappears while a domain is open.
; ---------------------------------------------------------------------------
	.area _DATA
	.ifndef KOS_MAP_DAT
_ipc_saved_latch::
	.ds	1
; The bulk-map span: 0 when closed, 80h | the replaced bank field when
; open; and the count of spans the endpoint return path had to close (a
; driver fault).
_ipc_bulk_open::
	.ds	1
_ipc_bulk_faults::
	.ds	2
	.endif
_ipc_saved_sp::
	.ds	2
	.ifndef KOS_MAP_DAT
_ipc_src::
	.ds	2
_ipc_dst::
	.ds	2
_ipc_len::
	.ds	2
_ipc_copy_req::
	.ds	6
	.endif

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
