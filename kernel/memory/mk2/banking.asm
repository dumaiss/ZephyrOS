; -----------------------------------------------------------------------------
; KonaOS mapping state and gates: the Zephyr-80 Mk2 implementation.
;
; The Mk2 counterpart of kernel/memory/banking.asm: the syscall gate's
; common state, the DAT state (mirrors, control, the working bank's image),
; the two DAT routines the mapping operations call, and the fatal path.
; Everything is common memory (logical E000h-FFFFh, the page entries 7 and
; 15 share), reachable from both banks.
; -----------------------------------------------------------------------------

	.module banking_mk2
	.include "hardware.inc"
	.include "memory.inc"
	.include "map_ops.inc"

	.globl __kos_mode_fault
	.globl kos_gate_saved_sp
	.globl kos_gate_saved_latch
	.globl kos_gate_result_hl
	.globl kos_gate_result_a
	.globl kos_gate_result_c
	.globl kos_gate_saved_iff
	.globl kos_gate_saved_a
	.globl kos_gate_stack
	.globl kos_gate_stack_top
	.globl _early_console_init
	.globl _panic

	.area _COMMON

; ---------------------------------------------------------------------------
; kos_dat_restore - KOS_MAP_RESTORE: install the mapping value in A.
;
; 00h selects the system bank.  Any other value selects the working bank;
; when a syscall crossing is open (the gate entered and has not left), the
; working bank first gets the caller's image back if the body loaded
; another one, and system entry 0 gets back the page the gate replaced
; with the caller's page 0.  The interrupted or calling context always
; runs from common memory, so no step hides the code or the stack.
;
; Interrupts masked.  Clobbers: AF.
; ---------------------------------------------------------------------------
kos_dat_restore::
	or	a
	jr	nz,dat_restore_program
	push	bc
	KOS_MAP_SYSTEM
	pop	bc
	ret
dat_restore_program:
	push	bc
	ld	a,(kos_gate_open)
	or	a
	jr	nz,dat_restore_close
	ld	a,#KOS_DAT_CTL_WORKING
	KOS_DAT_SELECT
	pop	bc
	ret
dat_restore_close:
	push	hl
	ld	hl,(kos_gate_saved_image)
	ld	a,(KOS_DAT_CACHED)
	cp	l
	jr	nz,dat_restore_load
	ld	a,(KOS_DAT_CACHED + 1)
	cp	h
	jr	z,dat_restore_loaded
dat_restore_load:
	call	kos_dat_load_working
dat_restore_loaded:
	pop	hl
	ld	a,#KOS_DAT_CTL_WORKING
	KOS_DAT_SELECT
	ld	a,(kos_gate_saved_page0)
	KOS_DAT_REMAP 0
	xor	a
	ld	(kos_gate_open),a
	pop	bc
	ret

; ---------------------------------------------------------------------------
; kos_dat_load_working - KOS_DAT_LOAD_WORKING: HL = an 8-byte image.
;
; Loads entries 8-15 one at a time, mirror first; the working bank holds
; no image until the last entry is in, then holds HL's.  The working bank
; must not be selected (its entries change under the program otherwise).
;
; Interrupts masked.  Clobbers: AF.
; ---------------------------------------------------------------------------
	.macro	DAT_LOAD_ENTRY n
	ld	a,(hl)
	KOS_DAT_SET KOS_DAT_ENTRIES+n
	.endm

kos_dat_load_working::
	push	hl
	xor	a
	ld	(KOS_DAT_CACHED + 1),a
	DAT_LOAD_ENTRY 0
	inc	hl
	DAT_LOAD_ENTRY 1
	inc	hl
	DAT_LOAD_ENTRY 2
	inc	hl
	DAT_LOAD_ENTRY 3
	inc	hl
	DAT_LOAD_ENTRY 4
	inc	hl
	DAT_LOAD_ENTRY 5
	inc	hl
	DAT_LOAD_ENTRY 6
	inc	hl
	DAT_LOAD_ENTRY 7
	pop	hl
	ld	(KOS_DAT_CACHED),hl
	ret

; ---------------------------------------------------------------------------
; __kos_mode_fault - a gate received a context it cannot serve (a syscall
; from the system bank).  Runs from common memory: the boot stack, the
; early console, the panic path.
; ---------------------------------------------------------------------------
__kos_mode_fault::
	ld	sp,#KOS_STACK_INIT_SP
	call	_early_console_init
	ld	hl,#_kos_str_mode_fault
	jp	_panic

_kos_str_mode_fault:
	.asciz	"mapping assertion failed"

; ---------------------------------------------------------------------------
; Gate state (common RAM).  Single-depth: the gate runs masked and never
; nests.  The first block is the Mk1 gate's; the second is the DAT state
; the syscall crossing saves (KOS_MAP_GATE_ENTER) and puts back
; (kos_dat_restore).
; ---------------------------------------------------------------------------
	.area _COMMON_DATA

kos_gate_saved_sp::
	.ds	2
kos_gate_saved_latch::		; the caller's mapping value (its page 0)
	.ds	1
kos_gate_result_hl::
	.ds	2
kos_gate_result_a::
	.ds	1
kos_gate_result_c::
	.ds	1
kos_gate_saved_iff::
	.ds	1
kos_gate_saved_a::
	.ds	1
kos_gate_saved_page0::		; system entry 0 before the call
	.ds	1
kos_gate_saved_image::		; the caller's working image
	.ds	2
kos_gate_open::			; 1 while a crossing is open
	.ds	1
kos_gate_image::		; the caller's bank when no image matched it
	.ds	KOS_DAT_ENTRIES
kos_gate_stack::
	.ds	KOS_GATE_STACK_SIZE
kos_gate_stack_top::

; The DAT state at its fixed address (memory.inc): the mirrors of both
; banks, the control mirror and the working bank's image.
	.area _DAT_STATE (ABS)
	.org KOS_DAT_STATE_BASE
kos_dat_state::
	.ds	KOS_DAT_STATE_SIZE
