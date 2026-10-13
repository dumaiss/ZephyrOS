; -----------------------------------------------------------------------------
; KonaOS libkos: exit, getinfo and reboot gate wrappers.
;
; These two syscalls are marshalled here; console I/O is not a syscall
; (user/sdk/src/console_proto.c speaks the console driver protocol).  Converts the private SDCC Z80 calling
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
	.globl _reboot
	.globl _exe_runtime_error
	.globl _kos_scratch

SYS_EXIT    = 0x00
SYS_GETINFO = 0x04
SYS_REBOOT  = 0x0C

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

; void reboot(void);  does not return.
_reboot::
	ld	hl,#_kos_scratch
	ld	a,#SYS_REBOOT
	rst	0x30
	; SYS_REBOOT never returns; trap a kernel contract violation.
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
