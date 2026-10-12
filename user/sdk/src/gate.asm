; -----------------------------------------------------------------------------
; KonaOS libkos: exit and getinfo gate wrappers.
;
; These two are the syscalls the SDK still marshals itself (the console ones
; were removed in package 41 - user/sdk/src/console_proto.c speaks the
; console driver protocol instead).  Converts the private SDCC Z80 calling
; convention (first 16-bit argument in HL, second in DE; 16-bit return in
; DE) to the native register ABI: A = syscall number, HL = argument block,
; carry clear with HL = result or carry set with A = errno.  The argument
; block is the shared low-window scratch (_kos_scratch in runtime.asm); a
; program stack near E000h is never used for it.
;
; The syscall numbers below are pinned against abi/kos/syscall.h by
; tests/test_sdk.py.
; -----------------------------------------------------------------------------

	.module kos_gate

	.globl _exit
	.globl _getinfo
	.globl _exe_runtime_error
	.globl _kos_scratch

SYS_EXIT    = 0x00
SYS_GETINFO = 0x04

	.area _CODE

; void exit(unsigned int status);  HL = status; does not return.
_exit::
	ld	(_kos_scratch),hl
	ld	a,#SYS_EXIT
	ld	hl,#_kos_scratch
	rst	0x30
	; SYS_EXIT never returns; trap a kernel contract violation.
	di
001$:
	halt
	jr	001$

; int getinfo(struct kos_info_record *record);  HL = record.
_getinfo::
	ld	(_kos_scratch+8),hl
	ld	a,#SYS_GETINFO
	ld	hl,#_kos_scratch
	rst	0x30
	jp	c,_exe_runtime_error
	ld	de,(_kos_scratch+8)
	ld	hl,#_kos_scratch
	ld	bc,#6
	ldir
	ld	de,#6
	ret
