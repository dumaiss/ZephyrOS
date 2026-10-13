; -----------------------------------------------------------------------------
; KonaOS common ROM-access primitive.
;
; Reads a bounded window (offset/length inside 0000h-BFFFh) of one
; configured recovery ROM page into the common staging buffer and restores
; the exact prior machine state.
;
; MEM_DECODER rev 12 truth: only mode 00 answers reads from ROM, and it
; answers every read from ROM (0000h-FFFFh), while every write targets the
; selected SRAM bank.  There is no mode that shows ROM and a SRAM common
; region at the same time; 08h is a flat view of one SRAM bank.  The
; transition therefore follows the machine's drive-A pattern: a two-byte
; trampoline OUT enters mode 00, execution continues in a stub that is
; byte-identical in common SRAM and in every recovery ROM page, the stub
; copies ROM -> staging (bank 0) and restores the caller latch, and only
; then does ordinary common code run again.
;
; Contract (see docs/romfs-format.md):
;   - the request is validated before any mapping changes;
;   - interrupts are locked (IFF saved and restored exactly);
;   - the exact caller latch is saved and restored byte-for-byte;
;   - no instruction, stack byte, or return address is used from SRAM
;     while ROM is visible: the stub is stackless and every fetch between
;     the two OUTs comes from the mirrored stub in the ROM page;
;   - the copy uses only the bounded common staging buffer (SRAM bank 0);
;   - the routine is non-reentrant (busy flag).
;
; Entry: HL = const struct rom_access_req * (page:u8, offset:u16, length:u16)
; Exit:  A = 0 (staged OK), KOS_EINVAL (bad request), KOS_EBUSY (reentry).
;        On success the staged bytes are in rom_access_staging.
; Clobbers: AF, AF', BC, DE, HL.  Interrupts restored to their entry state.
; -----------------------------------------------------------------------------

	.module rom_access
	.include "hardware.inc"
	.include "memory.inc"

; Status codes returned in A.  They are the positive KOS_* values from
; abi/kos/errno.h; tests/test_romfs.py enforces the parity so
; the two representations cannot drift.
RAX_EINVAL = 0x02		; KOS_EINVAL
RAX_EBUSY  = 0x04		; KOS_EBUSY

	.globl _rom_access_copy
	.globl _rom_access_staging
	.globl rom_access_trampoline
	.globl rom_access_sync
	.globl rom_access_sync_end
	.globl rom_access_saved_latch
	.globl rom_access_saved_iff
	.globl rom_access_busy

	.area _COMMON

; uint8_t rom_access_copy(const struct rom_access_req *req);
; (SDCC prefixes C identifiers with one underscore.)
_rom_access_copy::
	; ---- read and validate the request (mode 11, bank 7 visible) ----
	ld	a,h
	or	l
	jp	z, rax_invalid		; null request
	ld	a,(hl)			; [0] page
	ld	c,a
	inc	hl
	ld	e,(hl)			; [1] offset lo
	inc	hl
	ld	d,(hl)			; [2] offset hi
	inc	hl
	ld	a,(hl)			; [3] length lo
	inc	hl
	ld	h,(hl)			; [4] length hi
	ld	l,a			; HL = length

	; length must be 1..KOS_ROMFS_STAGING_SIZE (never 0x100: LDIR would
	; run 65536 iterations).
	ld	a,h
	or	a
	jp	nz, rax_invalid
	ld	a,l
	or	a
	jp	z, rax_invalid
	cp	#(KOS_ROMFS_STAGING_SIZE + 1)
	jp	nc, rax_invalid

	; page must be one of the configured recovery pages.
	ld	a,c
	cp	#KOS_ROMFS_FIRST_PAGE
	jp	c, rax_invalid
	cp	#(KOS_ROMFS_FIRST_PAGE + KOS_ROMFS_PAGE_COUNT)
	jp	nc, rax_invalid

	; offset + length must stay inside the 0000h-BFFFh window without
	; 16-bit wrap (DE = offset, HL = length). Only flags and A are used:
	; DE must keep the pure offset for the state stash below.
	ld	a,e
	add	a,l
	ld	a,d
	adc	a,h
	jp	c, rax_invalid		; wrapped past 64 KiB
	cp	#(KOS_ROMFS_WINDOW_SIZE >> 8)
	jp	c, rax_window_ok	; high byte below the window
	jp	nz, rax_invalid		; high byte above the window
	ld	a,e
	add	a,l
	or	a
	jp	nz, rax_invalid		; sum == window size + low remainder
rax_window_ok:

	; ---- single-depth: refuse reentry before touching any state ----
	ld	a,(rom_access_busy)
	or	a
	jp	nz, rax_busy

	; ---- stash the validated request in common state ----
	ld	a,c
	ld	(rom_access_page),a
	ld	(rom_access_offset),de
	ld	(rom_access_length),hl

	; ---- save exact IFF state, then lock interrupts ----
	ld	a,i			; P/V = IFF2
	di
	ld	l,#0
	jp	po, rax_iff_captured
	inc	l
rax_iff_captured:
	ld	a,l
	ld	(rom_access_saved_iff),a

	; ---- save the exact caller latch and mark the transition busy ----
	in	a,(KOS_MEMCTL_PORT)
	ld	(rom_access_saved_latch),a
	ld	a,#1
	ld	(rom_access_busy),a

	; ---- arm the transition registers, then enter the trampoline ----
	; Mode 00 answers every read from the selected ROM page and every
	; write from SRAM bank 0, so the stub cannot touch the stack or read
	; any common byte: everything it needs travels in registers.
	;   HL = window source (ROM), DE = staging destination (bank 0),
	;   BC = length, A = mode-00 latch to write, and A' = exact caller
	;   latch, restored by the stub's closing OUT.
	ld	hl,(rom_access_offset)
	ld	de,#_rom_access_staging
	ld	bc,(rom_access_length)
	ld	a,(rom_access_saved_latch)
	ex	af,af'
	ld	a,(rom_access_page)
	rrca
	rrca
	rrca
	and	#KOS_ROM_PAGE_MASK	; A = page<<5 | mode 00 | bank 0
	jp	rom_access_trampoline

	; ---- post-window epilogue: back in mode 11 on the caller bank ----
rom_access_epilogue:
	xor	a
	ld	(rom_access_busy),a
	ld	a,(rom_access_saved_iff)
	or	a
	jr	z, rax_iff_restored
	ei				; takes effect after the next instruction
rax_iff_restored:
	xor	a			; status 0 = staged OK
	ret

rax_invalid:
	ld	a,#RAX_EINVAL
	ret

rax_busy:
	ld	a,#RAX_EBUSY
	ret

; ---------------------------------------------------------------------------
; Common RAM: staging buffer and saved transition state (physical SRAM
; bank 0; mode 11 keeps it at E000h-FFFFh, and mode 00 writes reach it
; because the stub's latch selects bank 0).  There is no transition
; stack: the mirrored stub is stackless by contract, and the caller's
; stack is only touched again after the mode-11 restore.
; ---------------------------------------------------------------------------
	.area _COMMON_DATA

_rom_access_staging::
	.ds	KOS_ROMFS_STAGING_SIZE
rom_access_saved_latch::
	.ds	1
rom_access_saved_iff::
	.ds	1
rom_access_page::
	.ds	1
rom_access_offset::
	.ds	2
rom_access_length::
	.ds	2
rom_access_busy::
	.db	0			; read before first write: define the reset state

; ---------------------------------------------------------------------------
; Fixed transition stub (ABS): mirrored into every recovery ROM page by
; tools/pack_image.py, so it is fetchable both before and after the mode
; change.  The trampoline's OUT is the last instruction before the stub;
; fetch then continues at rom_access_sync inside the ROM page, runs
; stackless, and the closing OUT restores the exact caller latch before
; any stack use.  tools/mapcheck.py pins the label spacing and the
; sync extent; tools/pack_image.py copies the sync bytes into the image.
; ---------------------------------------------------------------------------
	.area _ROM_ACCESS_STUB (ABS)
	.org	KOS_ROM_ACCESS_BASE - 2

rom_access_trampoline::
	out	(KOS_MEMCTL_PORT),a	; enter mode 00 on the selected page
rom_access_sync::
	ldir				; ROM (HL) -> SRAM bank 0 (DE)
	ex	af,af'			; A = exact caller latch
	out	(KOS_MEMCTL_PORT),a	; leave mode 00 before any stack read
	jp	rom_access_epilogue	; ordinary common code, back in mode 11
rom_access_sync_end::
