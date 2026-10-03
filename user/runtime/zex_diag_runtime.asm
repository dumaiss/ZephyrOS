; -----------------------------------------------------------------------------
; ZephyrOS M7 native diagnostic runtime: SYS_DIAG_* wrappers.
;
; Sibling of zex_runtime.asm (M6 console/getinfo wrappers).  A program
; links zex_runtime.asm plus this module when it needs the diagnostic
; services; the shared carry-to-errno mapper lives in zex_runtime.asm.
;
; The wrappers convert the private SDCC calling convention to the native
; register ABI and marshal every argument block through this module's
; own low-window scratch, so a program stack anywhere in its bank never
; violates the argument-block rule.  Bank/range/port validation is the
; kernel's concern; a rejected request surfaces as a negative result.
;
; Scratch layout (12 bytes, _DATA object placed at the start of BSS by
; the standard ZEX link layout):
;   +0..+7   diagnostic argument block / system-info record
;   +8..+9   caller destination-pointer stash
;
; System call numbers (native ABI, fixed by M7):
;   28 MEM_READ   29 MEM_WRITE   2A IO_IN   2B IO_OUT
;   2C BANK_INFO  2D SYSTEM_INFO
; -----------------------------------------------------------------------------

	.module zex_diag_runtime

	.globl _zex_diag_mem_read
	.globl _zex_diag_mem_write
	.globl _zex_diag_io_in
	.globl _zex_diag_io_out
	.globl _zex_diag_bank_info
	.globl _zex_diag_system_info
	.globl _zex_runtime_error
	.globl _zex_diag_scratch
	.globl zex_diag_scratch

; Native syscall numbers (kept value-identical with
; include/zephyros/syscall.h; tests pin the pair).
SYS_DIAG_MEM_READ    = 0x28
SYS_DIAG_MEM_WRITE   = 0x29
SYS_DIAG_IO_IN       = 0x2A
SYS_DIAG_IO_OUT      = 0x2B
SYS_DIAG_BANK_INFO   = 0x2C
SYS_DIAG_SYSTEM_INFO = 0x2D

	.area _CODE

; int zex_diag_mem_read(struct zex_diag_mem_read *req);  HL = req.
; Result: byte in DE, or -errno.
_zex_diag_mem_read::
	ld	de,#_zex_diag_scratch
	ld	bc,#4
	ldir				; req -> scratch (HL advanced past it)
	ld	a,#SYS_DIAG_MEM_READ
	ld	hl,#_zex_diag_scratch
	rst	0x30
	jp	c,_zex_runtime_error
	ex	de,hl				; byte -> DE
	ret

; int zex_diag_mem_write(struct zex_diag_mem_write *req);  HL = req.
; Result: 0 in DE, or -errno.
_zex_diag_mem_write::
	ld	de,#_zex_diag_scratch
	ld	bc,#4
	ldir
	ld	a,#SYS_DIAG_MEM_WRITE
	ld	hl,#_zex_diag_scratch
	rst	0x30
	jp	c,_zex_runtime_error
	ex	de,hl				; 0 -> DE
	ret

; int zex_diag_io_in(struct zex_diag_io *req);  HL = req (port used).
; Result: byte in DE, or -errno.
_zex_diag_io_in::
	ld	a,(hl)
	ld	(_zex_diag_scratch),a
	ld	a,#SYS_DIAG_IO_IN
	ld	hl,#_zex_diag_scratch
	rst	0x30
	jp	c,_zex_runtime_error
	ex	de,hl
	ret

; int zex_diag_io_out(struct zex_diag_io *req);  HL = req.
; Result: 0 in DE, or -errno.
_zex_diag_io_out::
	ld	a,(hl)
	ld	(_zex_diag_scratch),a
	inc	hl
	ld	a,(hl)
	ld	(_zex_diag_scratch+1),a
	ld	a,#SYS_DIAG_IO_OUT
	ld	hl,#_zex_diag_scratch
	rst	0x30
	jp	c,_zex_runtime_error
	ex	de,hl
	ret

; int zex_diag_bank_info(struct zex_bank_info *out);  HL = out.
; Writes {bank, mask}; result 2 in DE, or -errno.
_zex_diag_bank_info::
	ld	(_zex_diag_scratch+8),hl
	ld	a,#SYS_DIAG_BANK_INFO
	ld	hl,#0x0000		; no argument block
	rst	0x30
	jp	c,_zex_runtime_error
	ld	de,(_zex_diag_scratch+8)
	ld	a,l
	ld	(de),a			; bank
	inc	de
	ld	a,h
	ld	(de),a			; mask
	ld	de,#2
	ret

; int zex_diag_system_info(struct zex_system_info *out);  HL = out.
; The kernel writes the eight-byte record at the argument block; the
; wrapper then copies it to the caller.  Result 8 in DE, or -errno.
_zex_diag_system_info::
	ld	(_zex_diag_scratch+8),hl
	ld	a,#SYS_DIAG_SYSTEM_INFO
	ld	hl,#_zex_diag_scratch
	rst	0x30
	jp	c,_zex_runtime_error
	ld	de,(_zex_diag_scratch+8)
	ld	hl,#_zex_diag_scratch
	ld	bc,#8
	ldir
	ld	de,#8
	ret

	.area _DATA

; The diagnostic runtime's low-window syscall argument block; the
; manifest-checked symbol `zex_diag_scratch` proves it stays inside
; 0100h-1FFFh.  Both the C-mangling form and the readable name are
; exported so the map check can use the readable name.
_zex_diag_scratch::
zex_diag_scratch::
	.ds	12
