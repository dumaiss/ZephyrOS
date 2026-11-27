; -----------------------------------------------------------------------------
; KonaOS Zephyr-80 Mk2 bootstrap, common boot entry and C runtime setup
; (architecture 2.7.5, 2.7.6; docs/boot.md section 10).
;
; The linked image's logical page L is ROM page L (tools/pack_image.py):
; ROM page 00h is the reset and boot view, 01h-04h the nucleus (the fixed
; region and cold set 0), 05h-06h cold set 1, 07h the common page.  At reset the mapper is in pass mode: logical 0000h-FFFFh
; reads ROM 00000h-0FFFFh and nothing is writable, so the bootstrap is
; stackless until the common page is in SRAM:
;
;   1. program all sixteen entries (the boot DAT below) from ROM page 00h;
;   2. enable mapping on the system bank: entry 0 is ROM page 00h, so the
;      next fetch is the same ROM byte;
;   3. copy every nucleus page (the fixed region and the cold sets) and
;      the common page from ROM to its SRAM page, one 8 KiB page at a
;      time, through the source copy window
;      (entry 5, the ROM page) and the destination window (entry 6, the
;      SRAM page);
;   4. build the RAM system page-0 image (SRAM KOS_SYSTEM_PAGE0) in the
;      destination window: 0000h-00FFh zero, the 0030h syscall vector and
;      the 0066h NMI entry, the stubs every page-0 image carries;
;   5. jump into the common page, which loads the final system DAT
;      (entry 0 the page-0 image, entries 1-4 the nucleus's SRAM pages)
;      and the working bank's placeholder, mirror first, sets the stack,
;      the DAT state and the C runtime (each cold set's data zeroed with
;      the set mapped), and calls kernel_main.
;
; Boot exception (architecture 2.7.6): the pass-mode OUTs come before any
; RAM, so their mirror stores go nowhere; step 5 writes every entry again
; mirror first.  The IOC releases the Z80 with the RT clock stopped, so no
; NMI arrives before the stack exists.
;
; Handoff to kernel_main: system bank selected, mapping enabled, the
; mirrors equal the hardware, the working bank holds the placeholder and
; no image identity (the first exec loads its image), SP in common,
; interrupts masked.  kernel_main checks the system DAT itself.
;
; Area order fixes the linker allocation of the unbased areas:
;
;     _CODE (2000h) -> _HOME -> _INITIALIZER -> _GSINIT -> _GSFINAL
;                   -> _DATA -> _INITIALIZED          fixed: 2000h-5EFFh
;     _WINDOW (6000h) -> _WINDOW_DATA                  cold set 0: 6000h-9FFFh
;     _COLD1 (16000h) -> _COLD1_DATA                   cold set 1: 6000h-9FFFh
;     _COMMON (E000h) -> _COMMON_DATA
; -----------------------------------------------------------------------------

	.module start_mk2
	.include "hardware.inc"
	.include "memory.inc"
	.include "map_ops.inc"

	.area _CODE
	.area _HOME
	.area _INITIALIZER
	.area _GSINIT
	.area _GSFINAL
	.area _DATA
	.area _INITIALIZED
	.area _WINDOW
	.area _WINDOW_DATA
	.area _COLD1
	.area _COLD1_DATA
	.area _COMMON
	.area _COMMON_DATA

	.globl _kernel_main
	.globl _early_console_init
	.globl _panic
	.globl s__DATA, l__DATA
	.globl s__WINDOW_DATA, l__WINDOW_DATA
	.globl s__COLD1_DATA, l__COLD1_DATA
	.globl _kos_cold_reset
	.globl s__INITIALIZER, l__INITIALIZER
	.globl s__INITIALIZED

; ---- ROM page 00h: the reset view ------------------------------------------

	.area _MK2_BOOT (ABS)
	.org	0x0000
	di
	jp	boot_pass

; The NMI entry of the boot view (architecture 2.7.3).
	.org	KOS_NMI_ENTRY
	jp	KOS_RT_VECTOR

	.org	KOS_PAGE_ZERO_END

; One pass-mode entry write from the table at HL (the page on D0-D6 and
; A8-A14: OUT (n),A); the mirror store hits ROM and is lost.
	.macro	BOOT_ENTRY n
	ld	a,(hl)
	inc	hl
	KOS_DAT_SET n
	.endm

boot_pass:
	ld	hl,#boot_dat
	BOOT_ENTRY 0
	BOOT_ENTRY 1
	BOOT_ENTRY 2
	BOOT_ENTRY 3
	BOOT_ENTRY 4
	BOOT_ENTRY 5
	BOOT_ENTRY 6
	BOOT_ENTRY 7
	BOOT_ENTRY 8
	BOOT_ENTRY 9
	BOOT_ENTRY 10
	BOOT_ENTRY 11
	BOOT_ENTRY 12
	BOOT_ENTRY 13
	BOOT_ENTRY 14
	BOOT_ENTRY 15
	ld	a,#KOS_DAT_CTL_SYSTEM
	KOS_DAT_SELECT
	; Mapped, system bank: entry 7 is the SRAM common page.
	ld	hl,#boot_copies
boot_copy:
	ld	a,(hl)
	cp	#0xFF
	jr	z,boot_page0
	KOS_DAT_SET KOS_DAT_COPY_SRC_ENTRY
	inc	hl
	ld	a,(hl)
	KOS_DAT_SET KOS_DAT_COPY_DST_ENTRY
	inc	hl
	exx
	ld	hl,#KOS_DAT_COPY_SRC_BASE
	ld	de,#KOS_DAT_COPY_DST_BASE
	ld	bc,#KOS_DAT_PAGE_SIZE
	ldir
	exx
	jr	boot_copy
boot_page0:
	ld	a,#KOS_SYSTEM_PAGE0
	KOS_DAT_SET KOS_DAT_COPY_DST_ENTRY
	ld	hl,#KOS_DAT_COPY_DST_BASE
	ld	de,#KOS_DAT_COPY_DST_BASE + 1
	ld	bc,#KOS_PAGE_ZERO_END - 1
	ld	(hl),#0
	ldir
	ld	hl,#0x0030
	ld	de,#KOS_DAT_COPY_DST_BASE + 0x0030
	ld	bc,#3
	ldir
	ld	hl,#KOS_NMI_ENTRY
	ld	de,#KOS_DAT_COPY_DST_BASE + KOS_NMI_ENTRY
	ld	bc,#3
	ldir
	jp	boot_common_entry

; The boot DAT.  System bank: ROM pages 00h-04h identity (00h runs this
; code across the MAP_ENABLE write), the first copy pair in the windows,
; the SRAM common page.  Working bank: the final placeholder.
boot_dat:
	.db	0x00, 0x01, 0x02, 0x03, 0x04
	.db	0x01, KOS_NUCLEUS_FIRST_PAGE, KOS_COMMON_PAGE
	.db	KOS_SYSTEM_PAGE0, KOS_NUCLEUS_FIRST_PAGE
	.db	KOS_NUCLEUS_FIRST_PAGE + 1, KOS_NUCLEUS_FIRST_PAGE + 2
	.db	KOS_NUCLEUS_FIRST_PAGE + 3, 0x00, 0x00, KOS_COMMON_PAGE

; ROM page -> SRAM page, FFh-terminated: the nucleus pages (the fixed
; region and cold set 0), cold set 1 and the common page (its code,
; initialized data and the vector cells the image holds).
boot_copies:
	.db	KOS_NUCLEUS_FIRST_ENTRY + 0, KOS_NUCLEUS_FIRST_PAGE + 0
	.db	KOS_NUCLEUS_FIRST_ENTRY + 1, KOS_NUCLEUS_FIRST_PAGE + 1
	.db	KOS_NUCLEUS_FIRST_ENTRY + 2, KOS_NUCLEUS_FIRST_PAGE + 2
	.db	KOS_NUCLEUS_FIRST_ENTRY + 3, KOS_NUCLEUS_FIRST_PAGE + 3
	.db	KOS_COLD_FIRST_ROM_PAGE + 2, KOS_COLD_FIRST_PAGE + 2
	.db	KOS_COLD_FIRST_ROM_PAGE + 3, KOS_COLD_FIRST_PAGE + 3
	.db	KOS_DAT_ENTRIES - 1, KOS_COMMON_PAGE
	.db	0xFF

	.ifne	KOS_NUCLEUS_ENTRIES - 4
	.error	1	; boot_dat and boot_copies list four nucleus pages
	.endif
	.ifne	KOS_COLD_SETS - 2
	.error	1	; boot_copies, the page list and boot_crt0 list two sets
	.endif
	.ifne	KOS_COLD_FIRST_PAGE - KOS_NUCLEUS_FIRST_PAGE - 2
	.error	1	; cold set 0 is nucleus entries 3-4
	.endif

; ---- the nucleus's SRAM pages (pages.h kos_pages_init) ---------------------

	.area _CODE
_kos_nucleus_pages::
	.db	KOS_SYSTEM_PAGE0
	.db	KOS_NUCLEUS_FIRST_PAGE + 0, KOS_NUCLEUS_FIRST_PAGE + 1
	.db	KOS_NUCLEUS_FIRST_PAGE + 2, KOS_NUCLEUS_FIRST_PAGE + 3
	.db	KOS_COLD_FIRST_PAGE + 2, KOS_COLD_FIRST_PAGE + 3
	.db	KOS_COMMON_PAGE
	.db	0x00

; ---- common: the final DAT, the stack, the C runtime ------------------------

	.area _COMMON

; A final entry, mirror first (KOS_DAT_SET).
	.macro	FINAL_ENTRY n, pg
	ld	a,#pg
	KOS_DAT_SET n
	.endm

boot_common_entry:
	; Entry 0 changes under no code: this runs in common.
	FINAL_ENTRY 0, KOS_SYSTEM_PAGE0
	FINAL_ENTRY 1, KOS_NUCLEUS_FIRST_PAGE+0
	FINAL_ENTRY 2, KOS_NUCLEUS_FIRST_PAGE+1
	FINAL_ENTRY 3, KOS_NUCLEUS_FIRST_PAGE+2
	FINAL_ENTRY 4, KOS_NUCLEUS_FIRST_PAGE+3
	FINAL_ENTRY KOS_DAT_COPY_SRC_ENTRY, 0x00
	FINAL_ENTRY KOS_DAT_COPY_DST_ENTRY, 0x00
	FINAL_ENTRY 7, KOS_COMMON_PAGE
	; The working bank: the placeholder, holding no process image.
	xor	a
	ld	(KOS_DAT_CACHED + 1),a
	FINAL_ENTRY 8, KOS_SYSTEM_PAGE0
	FINAL_ENTRY 9, KOS_NUCLEUS_FIRST_PAGE+0
	FINAL_ENTRY 10, KOS_NUCLEUS_FIRST_PAGE+1
	FINAL_ENTRY 11, KOS_NUCLEUS_FIRST_PAGE+2
	FINAL_ENTRY 12, KOS_NUCLEUS_FIRST_PAGE+3
	FINAL_ENTRY 13, 0x00
	FINAL_ENTRY 14, 0x00
	FINAL_ENTRY 15, KOS_COMMON_PAGE
	ld	hl,#0
	ld	(KOS_DAT_CACHED),hl
	ld	a,#KOS_DAT_CTL_SYSTEM
	KOS_DAT_SELECT
	ld	sp,#KOS_STACK_INIT_SP
	call	_early_console_init
	call	boot_crt0
	call	_kos_cold_reset
	call	_kernel_main
	ld	hl,#boot_str_returned
	call	_panic
boot_stop:
	di
	halt
	jr	boot_stop

boot_str_returned:
	.asciz	"kernel_main returned"

; The C runtime: zero _DATA, cold set 0's _WINDOW_DATA and, with set 1
; mapped at entries 3-4 for the store, set 1's _COLD1_DATA (the linker
; truncates its address to the 16 bits the set runs at), copy
; _INITIALIZER to _INITIALIZED, run _GSINIT.  Clobbers AF, BC, DE, HL.
boot_crt0:
	ld	hl,#s__DATA
	ld	bc,#l__DATA
	call	boot_zero
	ld	hl,#s__WINDOW_DATA
	ld	bc,#l__WINDOW_DATA
	call	boot_zero
	FINAL_ENTRY KOS_COLD_FIRST_ENTRY, KOS_COLD_FIRST_PAGE+2
	FINAL_ENTRY KOS_COLD_FIRST_ENTRY+1, KOS_COLD_FIRST_PAGE+3
	ld	hl,#s__COLD1_DATA
	ld	bc,#l__COLD1_DATA
	call	boot_zero
	FINAL_ENTRY KOS_COLD_FIRST_ENTRY, KOS_COLD_FIRST_PAGE
	FINAL_ENTRY KOS_COLD_FIRST_ENTRY+1, KOS_COLD_FIRST_PAGE+1
	ld	bc,#l__INITIALIZER
	ld	a,b
	or	a,c
	jr	z,boot_crt0_init
	ld	de,#s__INITIALIZED
	ld	hl,#s__INITIALIZER
	ldir
boot_crt0_init:
	jp	boot_gsinit

; HL = start, BC = length (may be 0).
boot_zero:
	ld	a,b
	or	a,c
	ret	z
	ld	(hl),#0
	dec	bc
	ld	a,b
	or	a,c
	ret	z
	ld	e,l
	ld	d,h
	inc	de
	ldir
	ret

	.area _GSINIT
boot_gsinit:

	.area _GSFINAL
	ret
