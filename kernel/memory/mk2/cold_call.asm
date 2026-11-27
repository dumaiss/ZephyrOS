; -----------------------------------------------------------------------------
; KonaOS cold-set calls: the Zephyr-80 Mk2 veneer runtime (architecture
; 2.7.6, config/residency-mk2.json).
;
; The nucleus's cold code is split into cold sets, 16 KiB page pairs all
; linked at 6000h-9FFFh; system entries 3-4 show one set at a time.  Every
; reference from outside a set to code inside it goes through a veneer
; that tools/mapcheck.py mk2 --link generates in the fixed region:
;
;   __cold<symbol>:  call kos_cold_call
;                    .dw  <symbol>
;                    .db  <the set's first page>, <its second page>
;
; kos_cold_call keeps the caller's return address and the pages entries
; 3-4 show in a frame on the veneer stack (fixed data), maps the set
; (mirror first), calls the target with the caller's registers and stack
; arguments exactly as a direct call would see them, then puts entries
; 3-4 back and returns to the caller with the target's A, DE and HL.
; So the syscall dispatcher (fixed) reaches each body in its set, and the
; sets reach each other, with nothing but the veneer knowing which pages
; hold what.
;
; The veneer stack is single-tasking nucleus state: a cross-set chain
; always unwinds in order, except a body that leaves the nucleus without
; returning (an event return, a process exit); the syscall dispatcher
; empties the stack at every entry, where no veneer frame can be live
; (kos_cold_reset).  Nothing an interrupt runs uses it.
;
; Interrupts: the target runs under the caller's state.  An interrupt
; between a mirror write and its OUT finds the mirror already describing
; the entry the OUT installs (the IRQ trampoline puts the mirror back).
;
; SDCC sdcccall(1): arguments in A, HL, DE and on the stack; results in
; A, DE, HL.  BC is not preserved.
; -----------------------------------------------------------------------------

	.module cold_call_mk2
	.include "hardware.inc"
	.include "memory.inc"
	.include "map_ops.inc"

	.globl kos_cold_call
	.globl _kos_cold_reset
	.globl _panic

; Frames: the caller's return address and the two pages it ran with.
COLD_FRAME = 4
COLD_DEPTH = 8

	.area _CODE

kos_cold_call::
	ld	(cold_a),a
	ld	(cold_hl),hl
	pop	hl			; the veneer's target and pages
	ld	c,(hl)
	inc	hl
	ld	b,(hl)
	inc	hl
	ld	(cold_target),bc
	ld	c,(hl)
	inc	hl
	ld	b,(hl)			; C, B = the set's pages for entries 3, 4
	pop	hl			; the caller's return address
	ld	(cold_ret),hl
	ld	a,(cold_depth)
	cp	#COLD_DEPTH
	jr	nc,cold_overflow
	inc	a
	ld	(cold_depth),a
	ld	hl,(cold_sp)
	ld	a,(cold_ret)
	ld	(hl),a
	inc	hl
	ld	a,(cold_ret + 1)
	ld	(hl),a
	inc	hl
	ld	a,(KOS_DAT_SYSTEM + KOS_COLD_FIRST_ENTRY)
	ld	(hl),a
	inc	hl
	ld	a,(KOS_DAT_SYSTEM + KOS_COLD_FIRST_ENTRY + 1)
	ld	(hl),a
	inc	hl
	ld	(cold_sp),hl
	ld	a,c
	KOS_DAT_SET KOS_COLD_FIRST_ENTRY
	ld	a,b
	KOS_DAT_SET KOS_COLD_FIRST_ENTRY+1
	ld	a,(cold_a)
	ld	hl,(cold_hl)
	call	cold_enter
	ld	(cold_a),a
	ld	(cold_hl),hl
	ld	hl,(cold_sp)
	dec	hl
	ld	a,(hl)
	KOS_DAT_SET KOS_COLD_FIRST_ENTRY+1
	dec	hl
	ld	a,(hl)
	KOS_DAT_SET KOS_COLD_FIRST_ENTRY
	dec	hl
	ld	b,(hl)
	dec	hl
	ld	c,(hl)			; BC = the caller's return address
	ld	(cold_sp),hl
	ld	hl,#cold_depth
	dec	(hl)
	push	bc
	ld	a,(cold_a)
	ld	hl,(cold_hl)
	ret

; The target, entered with the caller's A and HL: only BC is used.
cold_enter:
	ld	bc,(cold_target)
	push	bc
	ret

cold_overflow:
	ld	hl,#cold_str_overflow
	jp	_panic

cold_str_overflow:
	.asciz	"cold-set calls nested too deep"

; void kos_cold_reset(void);  empty the veneer stack (the syscall
; dispatcher's entry).  Clobbers: AF, HL.
_kos_cold_reset::
	ld	hl,#cold_stack
	ld	(cold_sp),hl
	xor	a
	ld	(cold_depth),a
	ret

	.area _DATA

cold_a:
	.ds	1
cold_hl:
	.ds	2
cold_target:
	.ds	2
cold_ret:
	.ds	2
cold_depth:
	.ds	1
cold_sp:
	.ds	2
cold_stack:
	.ds	COLD_FRAME * COLD_DEPTH
