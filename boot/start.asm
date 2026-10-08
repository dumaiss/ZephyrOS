; -----------------------------------------------------------------------------
; ZephyrOS common boot entry and SDCC C runtime initialization.
;
; The .area declarations at the top fix the linker allocation order for the
; areas that do not have an explicit base address:
;
;     _CODE (2000h) -> _HOME -> _INITIALIZER -> _GSINIT -> _GSFINAL
;     _DATA (derived base) -> _INITIALIZED
;     _WINDOW (6000h) -> _WINDOW_DATA (derived base)
;     _COMMON (E000h)
;
; All of _HOME/_INITIALIZER/_GSINIT/_GSFINAL therefore land inside the
; fixed core (2000h-5FFFh), and _DATA/_INITIALIZED land in the
; zero-initialized fixed runtime region below the stacks.  The _DATA base is
; derived per image (align_down(ZOS_SERVICE_STACK_BASE - data_size, 512)) and
; substituted by the build; the startup code below uses the linked s__/l__
; symbols, never a fixed address.  _WINDOW/_WINDOW_DATA are the cold half
; (6000h-DFFFh): _WINDOW holds the cold code, _WINDOW_DATA its uninitialized
; data and its base is derived (align_down(ZOS_BANK7_RUNTIME_END - size,
; 512)).  The cold code runs only through the window services, so it is
; neither copied nor executed at boot; _WINDOW_DATA must be zeroed by the
; startup path below just like _DATA.
;
; _boot_common_entry runs in mode 10 over bank 0, entirely from the
; permanent common region.  It establishes the common boot stack, enters
; mode 11 (bank 7 visible at 2000h-DFFFh), initializes the early console,
; prepares the SDCC C runtime, and calls kernel_main().  If kernel_main()
; ever returns that is a kernel fault and the panic path is taken.
;
; C runtime contract implemented here (see docs/boot.md):
;   1. the _DATA area (uninitialized globals and function-local statics) is
;      zeroed from s__DATA/l__DATA;
;   2. the _WINDOW_DATA area (uninitialized cold-half globals) is zeroed
;      from s__WINDOW_DATA/l__WINDOW_DATA;
;   3. the _INITIALIZER ROM image is copied to _INITIALIZED
;      (s__INITIALIZER/s__INITIALIZED/l__INITIALIZER);
;   4. compiler-emitted _GSINIT code (static initializers) is executed.
; No hosted CRT or standard library is linked.
; -----------------------------------------------------------------------------

	.module start
	.include "zephyr80/hardware.inc"
	.include "zephyr80/memory.inc"

	; Area declaration order = linker allocation order for unbased areas.
	.area _CODE
	.area _HOME
	.area _INITIALIZER
	.area _GSINIT
	.area _GSFINAL
	.area _DATA
	.area _INITIALIZED
	.area _WINDOW
	.area _WINDOW_DATA
	.area _COMMON
	.area _COMMON_DATA

	.globl _boot_common_entry
	.globl _kernel_main
	.globl _early_console_init
	.globl _panic
	.globl __zos_crt0_init
	.globl __zos_mode_fault
	; Linker-synthesized SDCC runtime area symbols.
	.globl s__DATA, l__DATA
	.globl s__WINDOW_DATA, l__WINDOW_DATA
	.globl s__INITIALIZER, l__INITIALIZER
	.globl s__INITIALIZED

	.area _COMMON

; Mode 10 over bank 0 is active; the common region E000h-FFFFh is visible.
; The Z80 starts with a fresh copy of this code from SRAM bank 0.
_boot_common_entry::
	; Common boot/runtime stack: FF00h-FFFFh, grown down from FFFFh.
	; SP = 0000h is top-exclusive at 10000h.
	ld	sp,#ZOS_STACK_INIT_SP

	; Enter OS mode over bank 0: 18h | 00h = 18h.  Bank 7 becomes
	; visible at 2000h-DFFFh; E000h-FFFFh stays in physical bank 0.
	; This code continues from the common region, which is mapped in
	; both modes.
	ld	a,#ZOS_MODE_OS
	out	(ZOS_MEMCTL_PORT),a

	; Boot-time latch assertion: mode 11 over bank 0 must be active
	; before any bank-7 code executes.  On failure, take the
	; common-resident fault path (console + halt).
	in	a,(ZOS_MEMCTL_PORT)
	and	#(ZOS_MODE_MASK | ZOS_BANK_MASK)
	cp	#ZOS_MODE_OS
	jp	nz,__zos_mode_fault

	; Early console first: the very first thing a broken boot can
	; report through is a common-resident console.
	call	_early_console_init

	; SDCC C runtime state (zero DATA, copy initializers, run GSINIT).
	call	__zos_crt0_init

	; kernel_main() does not return.
	call	_kernel_main

	; Kernel fault: kernel_main() returned.
	ld	hl,#_boot_str_kernel_main_returned
	call	_panic

; Unreachable fallback in case panic() ever returns.  Controlled halt.
_boot_stop::
	di
	halt
	jr	_boot_stop

; Must stay common-resident: it is the argument for the panic path.
_boot_str_kernel_main_returned:
	.asciz	"kernel_main returned"

; ---------------------------------------------------------------------------
; __zos_crt0_init - prepare the SDCC C runtime state actually used by the
; kernel.  Runs with bank 7 visible (mode 11).  Clobbers AF/BC/DE/HL.
; ---------------------------------------------------------------------------
__zos_crt0_init::
	; 1. Zero the _DATA area.
	ld	hl,#s__DATA
	ld	bc,#l__DATA
	call	__zos_zero_area
	; 2. Zero the _WINDOW_DATA area (cold-half uninitialized data).
	ld	hl,#s__WINDOW_DATA
	ld	bc,#l__WINDOW_DATA
	call	__zos_zero_area
	; 3. Copy the _INITIALIZER ROM image into _INITIALIZED.
	ld	bc,#l__INITIALIZER
	ld	a,b
	or	a,c
	jr	z,101$
	ld	de,#s__INITIALIZED
	ld	hl,#s__INITIALIZER
	ldir
101$:
	; 4. Run compiler-emitted static initializers (_GSINIT area).
	call	__zos_gsinit
	ret

; ---------------------------------------------------------------------------
; __zos_zero_area - zero a runtime data area.  HL = area start, BC = length.
; Clobbers AF/BC/DE/HL.  Called with bank 7 visible, so the fixed _DATA area
; and the cold _WINDOW_DATA area are both addressable.
; ---------------------------------------------------------------------------
__zos_zero_area:
	ld	a,b
	or	a,c
	ret	z
	dec	bc
	ld	(hl),#0x00
	ld	a,b
	or	a,c
	ret	z
	ld	e,l
	ld	d,h
	inc	de
	ldir
	ret

	.area _GSINIT
; Label at the head of the _GSINIT area: compiler-emitted static
; initializer code from the C objects is concatenated after this label.
__zos_gsinit::

	.area _GSFINAL
	ret
