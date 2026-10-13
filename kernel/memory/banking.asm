; -----------------------------------------------------------------------------
; KonaOS platform banking primitives and mode gates.
;
; Everything here is resident in the permanent common region (physical bank
; 0, E000h-FEFFh):
;   code  -> _COMMON        (placed at E000h by the linker script)
;   state -> _COMMON_DATA   (immediately after _COMMON, below the FF00h stack)
;
; Rules honored by every routine in this file:
;   - an instruction that changes the memory mapping, and everything
;     executed immediately after it, stays in common memory;
;   - no routine uses a stack or return address that its own mapping
;     change could hide.
;
; Latch (port 00h): D2..D0 selected SRAM bank, D4:D3 mode
;                   (00h ROM, 08h flat, 10h application, 18h OS),
;                   D7:D5 selected ROM page.
;
; Register clobbers are documented per routine.  These helpers are internal
; KonaOS platform mechanisms, not a stable ABI.
; -----------------------------------------------------------------------------

	.module banking
	.include "hardware.inc"
	.include "memory.inc"

	.globl _platform_memctl_read
	.globl _platform_memctl_write
	.globl _platform_reboot_now
	.globl _platform_bank_get
	.globl _platform_bank_set
	.globl kos_enter_os_mode
	.globl kos_leave_os_mode
	.globl kos_os_continuation
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
	; External (kernel/boot) entry points used by the gates.
	.globl _kernel_main
	.globl _early_console_init
	.globl _panic

	.area _COMMON

; ---------------------------------------------------------------------------
; C-callable latch helpers (SDCC sdcccall(1): first uint8_t argument in A,
; uint8_t result in L).  Mode-preserving, safe to call from bank-7 code in
; mode 11: the banked code region 2000h-DFFFh always maps bank 7 in OS mode.
; ---------------------------------------------------------------------------

; uint8_t platform_memctl_read(void);  clobbers A, L
_platform_memctl_read::
	in	a,(KOS_MEMCTL_PORT)
	ld	l,a
	ret

; void platform_memctl_write(uint8_t value);  A = value; clobbers A
; WARNING: may change the mapping and hide the caller.  Only common-resident
; code (the gates below) may use this to alter mode bits.
_platform_memctl_write::
	out	(KOS_MEMCTL_PORT),a
	ret

; uint8_t platform_bank_get(void);  clobbers A, L
_platform_bank_get::
	in	a,(KOS_MEMCTL_PORT)
	and	#KOS_BANK_MASK
	ld	l,a
	ret

; void platform_bank_set(uint8_t bank);  A = bank; clobbers AF, C
; Preserves ROM-page and mode bits; rejects bank 7 (application banks are
; 0-6; bank 7 is reserved to KonaOS).  A rejected value leaves the latch
; untouched.
_platform_bank_set::
	and	#KOS_BANK_MASK
	cp	#KOS_BANK_MASK		; 7 -> reject
	ret	z
	ld	c,a
	in	a,(KOS_MEMCTL_PORT)
	and	#KOS_LATCH_NON_BANK_MASK
	or	c
	out	(KOS_MEMCTL_PORT),a
	ret

; ---------------------------------------------------------------------------
; Mode 10 -> mode 11 gate (kernel-initiated entry).
;
; This is an assembly gate with an explicit common-memory continuation, not
; an ordinary C function: a C return address in 2000h-DFFFh would disappear
; as soon as OS mode is entered.  The selected application bank is taken
; from the hardware latch itself so the caller identity can never be
; invented in software.
;
; Entry: mode 10 active; SP may point anywhere, including application
;        memory in 2000h-DFFFh.
; Uses:  saves the caller SP and full latch in common state, switches to
;        the common gate stack, writes 18h|bank, then jumps to the bank-7
;        kernel body through the common continuation.
; Clobbers: AF, HL; SP is switched to the gate stack.  Does not return.
; ---------------------------------------------------------------------------
kos_enter_os_mode::
	; Save caller SP without using the stack.
	ld	hl,#0x0000
	add	hl,sp
	ld	(kos_gate_saved_sp),hl
	; Capture and validate the caller latch.
	in	a,(KOS_MEMCTL_PORT)
	ld	(kos_gate_saved_latch),a
	and	#KOS_MODE_MASK
	cp	#KOS_MODE_APPLICATION
	jp	nz,__kos_mode_fault	; fault path is common-resident
	ld	a,(kos_gate_saved_latch)
	and	#KOS_BANK_MASK
	cp	#KOS_BANK_MASK		; bank 7 is not an application bank
	jp	z,__kos_mode_fault
	; Switch to the common gate stack BEFORE the mode changes.
	ld	sp,#kos_gate_stack_top
	; Enter OS mode over the caller bank.  The next instructions are
	; fetched from common memory, which is mapped in both modes.
	or	#KOS_MODE_OS
	out	(KOS_MEMCTL_PORT),a
kos_os_continuation::
	jp	_kernel_main		; the boot path continues into the
					; kernel in mode 11

; ---------------------------------------------------------------------------
; Mode 11 -> mode 10 gate.
;
; Entry: mode 11 active; SP anywhere valid (the common gate stack).
; Uses:  restores the saved caller bank, writes 10h|bank, restores the
;        caller SP, then RETs to the caller's saved PC (pushed by RST 30h
;        and left on the caller stack while the kernel runs).
;        Abandons its own return address; does not return to its caller.
; Clobbers: AF, HL; SP restored to the caller SP.
; ---------------------------------------------------------------------------
kos_leave_os_mode::
	ld	a,(kos_gate_saved_latch)
	and	#KOS_BANK_MASK
	or	#KOS_MODE_APPLICATION
	out	(KOS_MEMCTL_PORT),a	; mode 10 over caller bank; next is common
	ld	hl,(kos_gate_saved_sp)
	ld	sp,hl			; caller stack is visible again in mode 10
	ret				; pop the caller's saved PC

; ---------------------------------------------------------------------------
; platform_reboot_now - warm reboot.  Never returns: takes no argument and
; leaves no state, and the ROM reset vector re-copies the OS exactly as at
; power-on, so nothing in RAM is trusted afterwards.
;
; The caller has masked interrupts and quiesced the interrupt sources.  The
; latch write selects mode 00 over ROM page 0, and from that instruction on
; every fetch comes from the ROM page: execution continues here because the
; common region is byte-identical in ROM page 0 and in the boot copy in
; SRAM bank 0 (the kernel never writes it), so the instruction after the OUT
; is the same byte either way.  The reset vector then re-runs the copier.
;
; Must stay in common memory; no stack is used after the latch write.
; Clobbers: AF.
; ---------------------------------------------------------------------------
_platform_reboot_now::
	di
	ld	a,#KOS_MODE_BOOT
	out	(KOS_MEMCTL_PORT),a
	jp	0x0000

; ---------------------------------------------------------------------------
; __kos_mode_fault - latch assertion failure path.
;
; Reached when the boot-time latch assertion fails or a gate receives an
; invalid bank.  Runs entirely from common memory: re-establishes the boot
; stack, initializes the early console, and enters the panic path.
; ---------------------------------------------------------------------------
__kos_mode_fault::
	ld	sp,#KOS_STACK_INIT_SP
	call	_early_console_init
	ld	hl,#_kos_str_mode_fault
	jp	_panic

_kos_str_mode_fault:
	.asciz	"banking latch assertion failed"

; ---------------------------------------------------------------------------
; Transition-critical state (common RAM, no ROM image).
; Single-depth: the gate runs with interrupts masked and never nests; the
; process side is shadowed per depth by the launch gate.
; ---------------------------------------------------------------------------
	.area _COMMON_DATA

kos_gate_saved_sp::
	.ds	2
kos_gate_saved_latch::
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
kos_gate_stack::
	.ds	KOS_GATE_STACK_SIZE
kos_gate_stack_top::
