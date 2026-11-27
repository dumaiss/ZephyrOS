; -----------------------------------------------------------------------------
; KonaOS system-entry spans: the Zephyr-80 Mk2 nucleus's routines that save,
; remap and restore a run of system DAT entries from C (architecture 2.7.6).
;
; A span is a first system entry and a count.  The driver domain (entries
; 3-6 for a driver's call, handler or init), the in-process endpoints' cold
; set (entries 3-4) and the domain's page 0 for a bulk-map span or a
; boot entry (entry 0) are installed and put back through them.  Each entry is written mirror
; first, then with the wiring-agnostic entry write (KOS_DAT_SET_NEXT,
; map_ops.inc), so the mirrors describe the mapping in force or the
; one the next OUT installs at every instruction boundary: an interrupt
; that saves and restores the mirror between two entries puts back exactly
; what these routines were installing.  The caller must not run from, or
; keep its stack in, an entry it remaps.
;
; A save area is the span's first entry, its count and the pages the
; mirrors named, one per entry (KOS_DAT_SAVE_SIZE(count) bytes); restoring
; it writes every entry back, whatever the entries show by then.
;
; Interface (kernel-internal, kernel/memory/mk2/pages.h; span =
; KOS_DAT_SPAN(first, count), count 1-8):
;
;   uint8_t kos_dat_span_enter(const uint8_t *want, uint16_t span,
;                              uint8_t *save);
;       HL = want (one page per entry), DE = span, save on the stack.
;       Records the span, then remaps it to `want` unless every mirror
;       already names it.  Returns 1 when it remapped, 0 when not.
;   uint8_t kos_dat_span_force(const uint8_t *want, uint16_t span,
;                              uint8_t *save);
;       The same, always remapping (an interrupt that may have found an
;       entry between its mirror write and its OUT).  Returns 1.
;   void kos_dat_span_restore(const uint8_t *save);
;       HL = save.  Writes the recorded pages back.
;   void kos_personality_switch(const uint8_t *pages, uint16_t entry);
;       SYS_PERSONALITY's one-way switch to a whole working mapping (the
;       first routine below); it never returns.
;
; SDCC sdcccall(1); the stack argument is the callee's to remove.  Runs
; under the caller's interrupt state.  Clobbers: AF, BC, DE, HL.
; -----------------------------------------------------------------------------

	.module dat_map_mk2
	.include "hardware.inc"
	.include "memory.inc"
	.include "map_ops.inc"

	.globl _kos_dat_span_enter
	.globl _kos_dat_span_force
	.globl _kos_dat_span_restore
	.globl _kos_personality_switch
	.globl _kos_personality_switch_end
	.globl _kos_dat_running

	; The mirrors of entries 0-15 share one high byte (INC E steps
	; one, as in KOS_DAT_SET_NEXT).
	.ifgt	(KOS_DAT_SYSTEM & 0xFF) + 16 - 0x100
	.error	1
	.endif

	.area _CODE

; ---------------------------------------------------------------------------
; void kos_personality_switch(const uint8_t *pages, uint16_t entry);
;   HL = the eight pages of the personality's working mapping (logical
;   page n first at n), DE = the entry.  SYS_PERSONALITY's last step
;   (kernel/syscall/personality.c), entered with interrupts masked and
;   every interrupt and NMI source stopped.  It never returns.
;
;   The copy windows (system entries 5-6) get the running program's own
;   pages 5-6 first, so every system entry shows RAM that holds no list
;   page: entry 0 the caller's page 0, 1-2 this region, 3-4 the cold set,
;   5-6 the program's pages, 7 the common page.  The working bank then
;   gets the eight pages, mirror first (KOS_DAT_LOAD_WORKING); the common
;   page leaves it.  The final instruction must change the mapping and be
;   followed by the entry, so it selects the working bank from the two
;   bytes below the entry in the system mapping (KOS_DAT_SELECT_VIA): the
;   next fetch is the entry, in the personality's mapping.  The caller
;   checked that those two bytes are not this routine's.  Nothing touches
;   the stack after the last pop, so the two bytes may land on it.  The
;   entry starts with interrupts masked and IM 1.
; ---------------------------------------------------------------------------
_kos_personality_switch::
	push	de			; the entry
	push	hl			; the pages
	ld	hl,(_kos_dat_running)
	ld	de,#KOS_DAT_COPY_SRC_ENTRY
	add	hl,de			; the program's page 5
	ld	a,(hl)
	KOS_DAT_SET KOS_DAT_COPY_SRC_ENTRY
	inc	hl
	ld	a,(hl)
	KOS_DAT_SET KOS_DAT_COPY_DST_ENTRY
	pop	hl
	KOS_DAT_LOAD_WORKING
	pop	hl
	dec	hl
	dec	hl			; HL = the entry - 2
	im	1
	ld	a,#KOS_DAT_CTL_WORKING
	KOS_DAT_SELECT_VIA
_kos_personality_switch_end::

; HL = save, E = first, D = count: the header and the mirrors.
span_save:
	ld	(hl),e
	inc	hl
	ld	(hl),d
	inc	hl
	ld	c,d
	ld	b,#0			; BC = count
	ex	de,hl			; DE = the saved pages, L = first
	ld	a,l
	add	a,#<KOS_DAT_SYSTEM
	ld	l,a
	ld	h,#>KOS_DAT_SYSTEM	; HL = the first mirror
	ldir
	ret

_kos_dat_span_force::
	ld	a,#1
	jr	span_enter
_kos_dat_span_enter::
	xor	a
span_enter:				; A = force, HL = want, DE = span
	pop	bc			; the return address
	ex	(sp),hl			; HL = save; the slot holds want
	push	bc
	push	de
	push	af
	call	span_save
	pop	af
	pop	de			; E = first, D = count
	pop	bc
	pop	hl			; HL = want
	push	bc			; the argument is gone
	push	hl
	push	de
	ld	c,a			; C = the differences (force: 1)
	ld	b,d
	ld	a,e
	add	a,#<KOS_DAT_SYSTEM
	ld	e,a
	ld	d,#>KOS_DAT_SYSTEM	; DE = the first mirror
span_compare:
	ld	a,(de)
	xor	(hl)
	or	c
	ld	c,a
	inc	e
	inc	hl
	djnz	span_compare
	pop	hl			; L = first, H = count
	pop	de			; DE = want
	ld	a,c
	or	a
	ret	z			; A = 0: already mapped
	call	span_write
	ld	a,#1
	ret

_kos_dat_span_restore::
	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	inc	hl
	ex	de,hl			; DE = the pages, L = first, H = count
; DE = the pages, L = first, H = count (1-8): enter the unrolled writes at
; the block that leaves `count` of them.
span_write:
	ld	c,l			; C = the first entry, its port
	ld	a,#8
	sub	h
	add	a,a
	add	a,a
	add	a,a			; 8 bytes a block
	add	a,#<span_blocks
	ld	l,a
	adc	a,#>span_blocks
	sub	l
	ld	h,a
	push	hl			; the first block to run
	ld	a,c
	add	a,#<KOS_DAT_SYSTEM
	ld	l,a
	ld	h,#>KOS_DAT_SYSTEM	; HL = its mirror
	ret

span_blocks:
	KOS_DAT_SET_NEXT
	KOS_DAT_SET_NEXT
	KOS_DAT_SET_NEXT
	KOS_DAT_SET_NEXT
	KOS_DAT_SET_NEXT
	KOS_DAT_SET_NEXT
	KOS_DAT_SET_NEXT
	KOS_DAT_SET_NEXT
	ret
