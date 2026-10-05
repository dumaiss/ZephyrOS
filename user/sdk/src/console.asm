; -----------------------------------------------------------------------------
; ZephyrOS libzos: console, getinfo and exit gate wrappers.
;
; Converts the private SDCC Z80 calling convention (first 16-bit argument
; in HL, second in DE; 16-bit return in DE) to the native register ABI:
; A = syscall number, HL = argument block, carry clear with HL = result or
; carry set with A = errno.  The argument block is always the shared
; low-window scratch (_zos_scratch in runtime.asm); a program stack near
; E000h is never used for it.
;
; The syscall numbers below are pinned against abi/zos/syscall.h by
; tests/test_sdk.py.
; -----------------------------------------------------------------------------

	.module zos_console

	.globl _zex_exit
	.globl _zex_write
	.globl _zex_read
	.globl _zex_poll
	.globl _zex_getinfo
	.globl _zex_runtime_error
	.globl _zos_scratch

SYS_EXIT    = 0x00
SYS_WRITE   = 0x01
SYS_READ    = 0x02
SYS_POLL    = 0x03
SYS_GETINFO = 0x04

	.area _CODE

; void zex_exit(unsigned int status);  HL = status; does not return.
_zex_exit::
	ld	(_zos_scratch),hl
	ld	a,#SYS_EXIT
	ld	hl,#_zos_scratch
	rst	0x30
	; SYS_EXIT never returns; trap a kernel contract violation.
	di
001$:
	halt
	jr	001$

; int zex_write(const char *buf, unsigned int len);  HL = buf, DE = len.
_zex_write::
	ld	(_zos_scratch),hl
	ld	(_zos_scratch+2),de
	ld	a,#SYS_WRITE
	ld	hl,#_zos_scratch
	rst	0x30
	jp	c,_zex_runtime_error
	ex	de,hl
	ret

; int zex_read(char *buf, unsigned int len);  HL = buf, DE = len.
_zex_read::
	ld	(_zos_scratch),hl
	ld	(_zos_scratch+2),de
	ld	a,#SYS_READ
	ld	hl,#_zos_scratch
	rst	0x30
	jp	c,_zex_runtime_error
	ex	de,hl
	ret

; int zex_poll(void);
_zex_poll::
	ld	a,#SYS_POLL
	ld	hl,#0x0000
	rst	0x30
	jp	c,_zex_runtime_error
	ex	de,hl
	ret

; int zex_getinfo(struct zos_info_record *record);  HL = record.
_zex_getinfo::
	ld	(_zos_scratch+8),hl
	ld	a,#SYS_GETINFO
	ld	hl,#_zos_scratch
	rst	0x30
	jp	c,_zex_runtime_error
	ld	de,(_zos_scratch+8)
	ld	hl,#_zos_scratch
	ld	bc,#6
	ldir
	ld	de,#6
	ret
