; -----------------------------------------------------------------------------
; ZephyrOS M6 native program runtime: syscall wrappers.
;
; Exposes the stable register ABI (docs/abi.md) to C programs without
; publishing SDCC calling conventions: the wrappers convert the private
; C argument registers (HL, DE) to the native ABI, run RST 30h, map a
; carry-set error to -errno, and return 16-bit results in DE.
;
; Properties every wrapper guarantees:
;   - the syscall argument block is always the runtime scratch block,
;     which the standard ZEX link layout places at the start of BSS
;     (low window); program stacks near E000h are never used for blocks;
;   - bank 7 and the reserved windows are the kernel's concern; a failed
;     call surfaces as a negative result and no program state is hidden.
;
; The scratch block layout (8 bytes):
;   +0..+3  console argument block {buffer:u16, length:u16}
;   +0..+5  getinfo destination record (written by the kernel)
;   +6..+7  getinfo caller destination pointer
;
; System call numbers (native ABI, fixed by M6):
;   00 EXIT   01 CONSOLE_WRITE   02 CONSOLE_READ   03 CONSOLE_POLL
;   04 GETINFO
; -----------------------------------------------------------------------------

	.module zex_runtime

	.globl _zex_exit
	.globl _zex_write
	.globl _zex_read
	.globl _zex_poll
	.globl _zex_getinfo
	.globl _zos_runtime_scratch
	.globl zos_runtime_scratch

	.area _CODE

; void zex_exit(unsigned int status);  HL = status; does not return.
_zex_exit::
	ld	(_zos_runtime_scratch),hl
	ld	a,#0x00			; SYS_EXIT
	ld	hl,#_zos_runtime_scratch
	rst	0x30
	; SYS_EXIT never returns; trap a kernel contract violation.
	di
001$:
	halt
	jr	001$

; int zex_write(const char *buf, unsigned int len);  HL = buf, DE = len.
_zex_write::
	ld	(_zos_runtime_scratch),hl
	ld	(_zos_runtime_scratch+2),de
	ld	a,#0x01			; SYS_CONSOLE_WRITE
	ld	hl,#_zos_runtime_scratch
	rst	0x30
	jp	c,zex_runtime_error
	ex	de,hl			; count -> DE
	ret

; int zex_read(char *buf, unsigned int len);  HL = buf, DE = len.
_zex_read::
	ld	(_zos_runtime_scratch),hl
	ld	(_zos_runtime_scratch+2),de
	ld	a,#0x02			; SYS_CONSOLE_READ
	ld	hl,#_zos_runtime_scratch
	rst	0x30
	jp	c,zex_runtime_error
	ex	de,hl
	ret

; int zex_poll(void);
_zex_poll::
	ld	a,#0x03			; SYS_CONSOLE_POLL
	ld	hl,#0x0000		; no argument block
	rst	0x30
	jp	c,zex_runtime_error
	ex	de,hl
	ret

; int zex_getinfo(struct zex_info *record);  HL = record.
_zex_getinfo::
	ld	(_zos_runtime_scratch+6),hl
	ld	a,#0x04			; SYS_GETINFO
	ld	hl,#_zos_runtime_scratch
	rst	0x30
	jp	c,zex_runtime_error
	; Copy the six-byte record from the scratch block to the caller
	; (LDIR copies HL -> DE).
	ld	de,(_zos_runtime_scratch+6)
	ld	hl,#_zos_runtime_scratch
	ld	bc,#6
	ldir
	ld	de,#6
	ret

; Map a carry-set return (A = errno) to DE = -errno.
zex_runtime_error:
	ld	e,a
	ld	d,#0
	xor	a
	sub	e
	ld	e,a
	ld	a,#0
	sbc	a,d
	ld	d,a
	ret

	.area _DATA

; The runtime-owned syscall argument block.  The standard ZEX link
; layout places _DATA at the image end; the manifest-checked symbol
; `zos_runtime_scratch` proves it stays inside the argument window.
; Both the C-mangling form and the plain name are exported so the map
; check can use the readable name.
_zos_runtime_scratch::
zos_runtime_scratch::
	.ds	8
