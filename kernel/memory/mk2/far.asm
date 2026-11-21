; -----------------------------------------------------------------------------
; KonaOS far window: the Zephyr-80 Mk2 realization of a mapping handle.
;
; The far window is logical 8000h-BFFFh of a program, working entries 12
; and 13 (architecture 2.7.6).  SYS_XMEM_MAP and SYS_SEG_MAP (kernel/
; syscall/xmem.c) turn one of the caller's mappable objects or far-code
; segments into a mapping handle: they fill the object's row of
; _kos_far_table with the pages the window shows and return the handle,
; one byte, (row << 3) | generation.  The program opens the window with
; kos_far_map, which names no page: the handle is logical and this
; routine, in common memory, realizes it.  Handle 0 is the program's own
; memory (its image's entries 4 and 5), so passing back the handle a call
; returned closes what that call opened.
;
; Programs reach kos_far_map through the page-0 vector every program
; page-0 image carries (KOS_FAR_VECTOR, 0028h: RST 28h), _kos_far_vector
; below, which exec copies there (kernel/process/exec.c).
;
; _kos_far_current is the running program's handle; the process lifecycle
; keeps it per process across exec (kernel/process/process.c) and an event
; frame keeps it across a delivery (kernel/event/event_common.asm).
; -----------------------------------------------------------------------------

	.module far_mk2
	.include "hardware.inc"
	.include "memory.inc"
	.include "map_ops.inc"

	.globl _kos_dat_running

; Rows of the far table: one per extended-memory object (XMEM_MAX_OBJECTS,
; kernel/syscall/xmem.c), each {generation, page of 8000h, page of A000h};
; generation 0 is a row with no handle, 1-7 a live one.
KOS_FAR_ROWS     = 32
KOS_FAR_ROW_SIZE = 3

	.area _CODE

; The three bytes every program page-0 image carries at KOS_FAR_VECTOR
; (C name kos_far_vector): the far-window entry, RST 28h.
_kos_far_vector::
	.db	0xC3			; JP kos_far_map
	.dw	kos_far_map

	.area _COMMON

; ---------------------------------------------------------------------------
; kos_far_map - open the far window onto a mapping handle.
;
; Entry: the program's context (working bank), any interrupt state;
;        HL = a handle from SYS_XMEM_MAP or SYS_SEG_MAP (H = 0), or 0 for
;        the program's own pages.
; Exit:  carry clear, HL = the handle in force before; or carry set and
;        nothing changed (a handle that names no live row).
; The window, its mirrors and the handle change with interrupts masked;
; the caller's interrupt state comes back.  The mirrors are written before
; each OUT (KOS_DAT_REMAP).  With handle 0 the working bank is the running
; image again and the cached-image identity says so.
; Clobbers: AF, BC, DE.
; ---------------------------------------------------------------------------
kos_far_map::
	push	hl			; the handle
	ld	a,h
	or	l
	jr	nz,far_object
	ld	hl,(_kos_dat_running)
	ld	bc,#4			; the image's entries 4 and 5
	add	hl,bc
	jr	far_pages
far_object:
	ld	a,h			; (row << 3) | generation, H = 0
	or	a
	jr	nz,far_refuse
	ld	a,l
	and	#0x07
	jr	z,far_refuse
	ld	c,a			; C = the generation
	xor	l
	rrca
	rrca
	rrca
	ld	l,a			; HL = the row
	ld	d,h
	ld	e,l
	add	hl,hl
	add	hl,de			; KOS_FAR_ROW_SIZE * row
	ld	de,#_kos_far_table
	add	hl,de
	ld	a,(hl)
	cp	c
	jr	nz,far_refuse
	inc	hl
far_pages:
	ld	d,(hl)			; the page of 8000h
	inc	hl
	ld	e,(hl)			; the page of A000h
	; IFF2 is read up to three times: on the board an interrupt accepted
	; right after LD A,I makes that read report 0.
	ld	a,i
	jp	pe,far_iff
	ld	a,i
	jp	pe,far_iff
	ld	a,i
far_iff:
	di
	push	af			; P/V = the caller's IFF2
	ld	a,d
	KOS_DAT_REMAP KOS_DAT_FAR_ENTRY
	ld	a,e
	KOS_DAT_REMAP KOS_DAT_FAR_ENTRY+1
	pop	bc			; C = the caller's flags
	pop	de			; DE = the handle
	ld	hl,(_kos_far_current)
	ld	(_kos_far_current),de
	ld	a,d
	or	e
	jr	nz,far_done
	push	hl
	ld	hl,(_kos_dat_running)
	ld	(KOS_DAT_CACHED),hl
	pop	hl
far_done:
	push	bc
	pop	af
	jp	po,far_masked
	ei
far_masked:
	or	a
	ret
far_refuse:
	pop	hl
	scf
	ret

	.area _COMMON_DATA

; The running program's mapping handle (0: its own pages).
_kos_far_current::
	.ds	2
_kos_far_table::
	.ds	KOS_FAR_ROWS * KOS_FAR_ROW_SIZE
_kos_far_table_end::
