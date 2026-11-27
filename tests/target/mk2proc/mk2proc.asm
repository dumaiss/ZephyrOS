; -----------------------------------------------------------------------------
; Zephyr-80 Mk2 processes image (test-only; tests/test_mk2_processes.py).
;
; The nucleus's process and memory code built for Mk2 (the page allocator,
; the page copy, the far window, the launch gate, the process lifecycle,
; the page loader and the extended-memory family) with the transition
; sources of the Mk2 transitions image, around a bootstrap, a C syscall
; dispatcher (mk2proc_kernel.c) and one program.  The kernel launcher
; runs the program at depth 1; the tests drive every process from the
; simulator through the program's fragments: a syscall (p_call), the far
; window (p_far), a copy through plain pointers (p_copy) and a return from
; the entry (p_ret).
;
; Physical pages (system DAT after boot):
;
;   0 40h  the system page-0 image (a copy of ROM page 0)
;   1 41h  kernel code (_CODE, 2000h)
;   2 42h  kernel code (4000h), the ISR stack at 5F00h
;   3 43h  kernel data (_DATA, 6000h)
;   4 44h
;   5 --   the source copy window
;   6 --   the destination copy window
;   7 7Fh  common
;
; The nucleus's pages (40h-44h, 7Fh) are reserved from mk2proc_nucleus;
; every other SRAM page is the allocator's.  ROM page 0 holds the reset
; view, the low stubs, the program (0100h) and the bootstrap (1000h).
; -----------------------------------------------------------------------------

	.module	mk2proc
	.include "hardware.inc"
	.include "memory.inc"
	.include "irq.inc"
	.include "map_ops.inc"

	.globl	_irq_arch_init
	.globl	_mk2proc_main
	.globl	kos_far_map
	.globl	kos_syscall_entry, _kos_process_page_zero_vector
	.globl	s__COMMON_DATA, l__COMMON_DATA, s__DATA, l__DATA
	.globl	s__INITIALIZER, l__INITIALIZER, s__INITIALIZED

P_BSS		= 0x0100	; the program's BSS after its image

	.area	_CODE
	.area	_HOME
	.area	_INITIALIZER
	.area	_GSINIT
	.area	_GSFINAL
	.area	_DATA
	.area	_INITIALIZED
	.area	_PROG
	.area	_BOOT
	.area	_COMMON
	.area	_COMMON_DATA

; ---- ROM page 0: the reset view and the low stubs --------------------------

	.area	_MK2PROC_LOW (ABS)
	.org	0x0000
	di
	jp	boot

	.org	KOS_NMI_ENTRY
	jp	KOS_RT_VECTOR

; ---- the program (0100h of its own page 0; its bytes are ROM page 0's) ----

	.area	_PROG
p_entry::				; HL = the startup block
	jr	p_idle
p_idle::
	jr	p_idle
p_call::				; the registers are the syscall's
	rst	0x30
p_return::
	jr	p_return
p_far::					; HL = a mapping handle
	call	kos_far_map
p_far_done::
	jr	p_far_done
p_copy::				; HL -> DE, BC bytes, plain pointers
	ldir
p_copy_done::
	jr	p_copy_done
p_ret::					; at the entry's SP: return from the entry
	ret
p_end::

; ---- the bootstrap (ROM page 0, 1000h) --------------------------------------

	.area	_BOOT
; Pass mode, no RAM: program both banks with B = A (wiring-agnostic), map,
; copy ROM pages into SRAM through entry 1 (source) and entry 2
; (destination), then the final images.  The last loop remaps entry 0
; from ROM page 00h to its SRAM copy under the running code.
boot:
	ld	hl,#boot_image
	ld	c,#0
boot_entry:
	ld	a,(hl)
	ld	b,a
	out	(c),a
	inc	hl
	inc	c
	ld	a,c
	cp	#2 * KOS_DAT_ENTRIES
	jr	nz,boot_entry
	ld	bc,#KOS_DAT_CONTROL_PORT
	ld	a,#KOS_DAT_CTL_SYSTEM
	out	(c),a
	ld	hl,#copy_table
boot_copy:
	ld	a,(hl)
	cp	#0xFF
	jr	z,boot_final
	out	(KOS_DAT_SYSTEM_PORT + 1),a
	inc	hl
	ld	a,(hl)
	out	(KOS_DAT_SYSTEM_PORT + 2),a
	inc	hl
	exx
	ld	hl,#0x2000
	ld	de,#0x4000
	ld	bc,#KOS_DAT_PAGE_SIZE
	ldir
	exx
	jr	boot_copy
boot_final:
	ld	hl,#final_image
	ld	c,#0
boot_final_entry:
	ld	a,(hl)
	ld	b,a
	out	(c),a
	inc	hl
	inc	c
	ld	a,c
	cp	#2 * KOS_DAT_ENTRIES
	jr	nz,boot_final_entry
	jp	t_start

boot_image::
	.db	0x00, 0x01, 0x41, 0x43, 0x44, 0x45, 0x46, 0x7F
	.db	0x00, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x7F
copy_table::
	.db	0x01, 0x41		; kernel code, 2000h
	.db	0x02, 0x42		; kernel code, 4000h
	.db	0x07, 0x7F		; common
	.db	0x00, 0x40		; the system page-0 image
	.db	0xFF
; Working bank: a placeholder (the system image); the first launch loads
; the program's image.
final_image::
	.db	0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x7F
	.db	0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x7F

; ---- kernel data the build provides (system bank) ----------------------------

	.area	_CODE
; The nucleus's SRAM pages (pages.h kos_pages_init), 00h-terminated.
_mk2proc_nucleus::
	.db	0x40, 0x41, 0x42, 0x43, 0x44, 0x7F, 0x00
; The program (kernel/include/process.h struct process_program).
_mk2proc_program::
	.db	0x00			; ROM page
	.dw	p_entry			; offset in it: linked at its own 0100h
	.dw	p_end - p_entry		; image bytes
	.dw	P_BSS
	.dw	p_entry

; ---- common: start, RT handler, test state ----------------------------------

	.area	_COMMON

t_start::
	ld	sp,#KOS_STACK_INIT_SP
	; The mirrors, after the boot's OUTs (architecture 2.7.6: no NMI
	; before the bootstrap has a RAM stack).  The working bank holds no
	; process image.
	ld	hl,#final_image
	ld	de,#KOS_DAT_SYSTEM
	ld	bc,#2 * KOS_DAT_ENTRIES
	ldir
	ld	a,#KOS_DAT_CTL_SYSTEM
	ld	(KOS_DAT_CONTROL),a
	ld	hl,#0
	ld	(KOS_DAT_CACHED),hl
	ld	hl,#s__COMMON_DATA
	ld	bc,#l__COMMON_DATA
	call	t_zero
	; The one initialized cell of _COMMON_DATA (launch.asm): the 0030h
	; vector exec copies into every page zero.
	ld	a,#0xC3
	ld	(_kos_process_page_zero_vector),a
	ld	hl,#kos_syscall_entry
	ld	(_kos_process_page_zero_vector + 1),hl
	ld	hl,#s__DATA
	ld	bc,#l__DATA
	call	t_zero
	ld	bc,#l__INITIALIZER
	ld	a,b
	or	c
	jr	z,t_start_init
	ld	hl,#s__INITIALIZER
	ld	de,#s__INITIALIZED
	ldir
t_start_init:
	call	_irq_arch_init
	di
	call	_mk2proc_main
_t_launcher_done::
	jr	_t_launcher_done

; HL = start, BC = length (may be 0).
t_zero:
	ld	a,b
	or	c
	ret	z
	ld	(hl),#0
	inc	hl
	dec	bc
	jr	t_zero

; The RT owner's handler: two bytes of stack, no mapping change.
t_rt_tick::
	push	hl
	ld	hl,(t_rt_count)
	inc	hl
	ld	(t_rt_count),hl
	pop	hl
	retn

_irq_dispatch::
_early_console_init::
	ret
_panic::
	ld	(t_panic),hl
	halt

	.area	_COMMON_DATA
t_rt_count::
	.ds	2
t_panic::
	.ds	2

; The RT vector: the test owns the real-time interrupt.
	.area	_MK2PROC_RT (ABS)
	.org	KOS_RT_VECTOR
	jp	t_rt_tick
	.org	KOS_RT_IDLE
	retn
