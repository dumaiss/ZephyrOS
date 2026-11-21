; -----------------------------------------------------------------------------
; KonaOS mapping state and gates: the Zephyr-80 Mk2 implementation.
;
; The Mk2 counterpart of kernel/memory/banking.asm: the syscall gate's
; common state, the DAT state (mirrors, control, the working bank's image),
; the processes' DAT images, the DAT routines the mapping operations call,
; and the fatal path.  Everything is common memory (logical E000h-FFFFh,
; the page entries 7 and 15 share), reachable from both banks.
;
; Each process has one 8-byte DAT image (_kos_dat_images, indexed by pid):
; entry 0 is its page-0 page, entries 1-6 its other pages, entry 7 the
; common page.  _kos_dat_running names the running process's image; the
; working bank holds it unless the program opened its far window
; (kernel/memory/mk2/far.asm) or a syscall body loaded another image.
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

; The gate's DAT block (kos_gate_dat below).
KOS_GATE_DAT_SIZE = 12

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
; kos_dat_enter - KOS_MAP_APP: enter the running program's context.
;
; The working bank gets the running image unless it holds it already (the
; cached-image shortcut), then is selected.  System bank selected,
; interrupts masked.  Clobbers: AF.
; ---------------------------------------------------------------------------
kos_dat_enter::
	push	hl
	ld	hl,(_kos_dat_running)
	ld	a,(KOS_DAT_CACHED)
	cp	l
	jr	nz,dat_enter_load
	ld	a,(KOS_DAT_CACHED + 1)
	cp	h
	jr	z,dat_enter_loaded
dat_enter_load:
	call	kos_dat_load_working
dat_enter_loaded:
	pop	hl
	push	bc
	ld	a,#KOS_DAT_CTL_WORKING
	KOS_DAT_SELECT
	pop	bc
	ret

; ---------------------------------------------------------------------------
; kos_dat_gate_suspend - KOS_MAP_GATE_SUSPEND; kos_dat_gate_resume -
; KOS_MAP_GATE_RESUME.  The gate's DAT block (saved page 0, saved image,
; open flag, gate image: kos_gate_dat, KOS_GATE_DAT_SIZE bytes) is single-
; depth like the rest of the gate state, so a parent suspended in SYS_EXEC
; keeps its copy per depth (kos_dat_gate_shadow) and gets it back, its
; crossing open again, before its SYS_EXEC returns.  The child starts with
; the crossing closed: system entry 0 is the system page-0 image again and
; no restore reloads the parent's image under it.  At depth 0 (the kernel
; launcher) nothing is kept: the resume closes the crossing the exited
; child's SYS_EXIT left open.
;
; System bank, interrupts masked; the resume runs in the launch gate's
; stackless window with SP below the frame it restored, so the CALL and
; the routine's use of the stack stay in the gate stack's free part.
; Entry: A = the depth.  Clobbers: AF, BC, DE, HL.
; ---------------------------------------------------------------------------
kos_dat_gate_suspend::
	call	dat_gate_shadow		; HL = the depth's copy, Z at depth 0
	jr	z,dat_gate_close
	ex	de,hl
	ld	hl,#kos_gate_dat
	ld	bc,#KOS_GATE_DAT_SIZE
	ldir
dat_gate_close:
	ld	a,(kos_gate_open)
	or	a
	ret	z
	ld	a,(kos_gate_saved_page0)
	KOS_DAT_REMAP 0
	xor	a
	ld	(kos_gate_open),a
	ret

kos_dat_gate_resume::
	call	dat_gate_shadow
	jr	z,dat_gate_close
	ld	de,#kos_gate_dat
	ld	bc,#KOS_GATE_DAT_SIZE
	ldir
	ld	a,(kos_gate_saved_latch)	; the parent's page 0
	KOS_DAT_REMAP 0
	ret

; A = the depth -> HL = kos_dat_gate_shadow + KOS_GATE_DAT_SIZE * A; Z
; set at depth 0.  Clobbers: F, DE.
dat_gate_shadow:
	ld	l,a
	ld	h,#0
	add	hl,hl
	add	hl,hl			; 4 * index
	ld	d,h
	ld	e,l
	add	hl,hl			; 8 * index
	add	hl,de			; 12 * index = KOS_GATE_DAT_SIZE * index
	ld	de,#kos_dat_gate_shadow
	add	hl,de
	or	a
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
; The gate's DAT block: KOS_GATE_DAT_SIZE bytes in this order (the launch
; gate keeps it per depth, kos_dat_gate_suspend).
kos_gate_dat::
kos_gate_saved_page0::		; system entry 0 before the call
	.ds	1
kos_gate_saved_image::		; the caller's working image: its current
_kos_gate_saved_image::		; mapping, far window included
	.ds	2
kos_gate_open::			; 1 while a crossing is open
	.ds	1
kos_gate_image::		; the caller's bank when no image matched it
	.ds	KOS_DAT_ENTRIES
kos_gate_dat_end::
kos_gate_stack::
	.ds	KOS_GATE_STACK_SIZE
kos_gate_stack_top::

; The gate's DAT block per exec depth (KOS_EXEC_MAX_DEPTH, abi/kos/
; process.h; depth 0 unused).
kos_dat_gate_shadow::
	.ds	4 * KOS_GATE_DAT_SIZE

; One DAT image per process (PROCESS_TABLE_SIZE, kernel/include/
; process.h), indexed by pid, and the running process's image (0 while
; the nucleus runs no program).
_kos_dat_images::
	.ds	7 * KOS_DAT_ENTRIES
_kos_dat_images_end::
_kos_dat_running::
	.ds	2

; The DAT state at its fixed address (memory.inc): the mirrors of both
; banks, the control mirror and the working bank's image.
	.area _DAT_STATE (ABS)
	.org KOS_DAT_STATE_BASE
kos_dat_state::
	.ds	KOS_DAT_STATE_SIZE
