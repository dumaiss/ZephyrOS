; -----------------------------------------------------------------------------
; KonaOS system-entry mapping: the Zephyr-80 Mk2 nucleus's one routine that
; remaps a run of system DAT entries from C (architecture 2.7.6).
;
; The driver domain (system entries 3-6 for a driver's call, handler or
; init; entry 0 for a bulk-map span or a boot entry) and the in-process
; endpoints' cold set (entries 3-4) are installed and put back through
; it.  Each entry is written mirror first, then with the wiring-agnostic
; entry write (KOS_DAT_SET_AT), so the mirrors describe the mapping in
; force or the one the next OUT installs at every instruction boundary:
; an interrupt that saves and restores the mirror between two entries
; puts back exactly what this routine was installing.  The caller must
; not run from, or keep its stack in, an entry it remaps.
;
; Interface (kernel-internal, kernel/memory/mk2/pages.h):
;
;   void kos_dat_map(const uint8_t *pages, uint16_t span);
;       HL = the pages, one per entry; E = the first system entry (0-7),
;       D = the entry count (0 maps nothing).
;
; SDCC sdcccall(1).  Runs under the caller's interrupt state.  Clobbers:
; AF, BC, DE, HL.
; -----------------------------------------------------------------------------

	.module dat_map_mk2
	.include "hardware.inc"
	.include "memory.inc"
	.include "map_ops.inc"

	.globl _kos_dat_map

	.area _CODE

_kos_dat_map::
	ld	a,d
	or	a
	ret	z
	ld	c,e			; C = the entry
	ex	de,hl			; DE = the pages, H = the count
	ld	a,h
dat_map_entry:
	push	af			; the entries left
	ld	a,(de)
	KOS_DAT_SET_AT
	inc	de
	inc	c
	pop	af
	dec	a
	jr	nz,dat_map_entry
	ret
