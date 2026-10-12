; -----------------------------------------------------------------------------
; KonaOS libkos: recovery/diagnostic gate wrappers (SYS_DIAG_*).
;
; Sibling of console.asm.  Each wrapper marshals its argument block
; through the shared low-window scratch (_kos_scratch) so a program stack
; anywhere in its bank never violates the argument-block rule.  Bank,
; range and port validation is the kernel's concern; a rejected request
; surfaces as a negative result.
;
; Scratch use: +0..+7 block or system-info record, +8..+9 destination
; stash.  The syscall numbers are pinned against abi/kos/diag.h by
; tests/test_sdk.py.
; -----------------------------------------------------------------------------

	.module kos_diaggate

	.globl _diag_mem_read
	.globl _diag_mem_write
	.globl _diag_io_in
	.globl _diag_io_out
	.globl _diag_bank_info
	.globl _diag_system_info
	.globl _exe_runtime_error
	.globl _kos_scratch

SYS_DIAG_MEM_READ    = 0x28
SYS_DIAG_MEM_WRITE   = 0x29
SYS_DIAG_IO_IN       = 0x2A
SYS_DIAG_IO_OUT      = 0x2B
SYS_DIAG_BANK_INFO   = 0x2C
SYS_DIAG_SYSTEM_INFO = 0x2D

	.area _CODE

; int diag_mem_read(struct kos_diag_mem_read_args *req);  HL = req.
_diag_mem_read::
	ld	de,#_kos_scratch
	ld	bc,#4
	ldir
	ld	a,#SYS_DIAG_MEM_READ
	ld	hl,#_kos_scratch
	rst	0x30
	jp	c,_exe_runtime_error
	ex	de,hl
	ret

; int diag_mem_write(struct kos_diag_mem_write_args *req);  HL = req.
_diag_mem_write::
	ld	de,#_kos_scratch
	ld	bc,#4
	ldir
	ld	a,#SYS_DIAG_MEM_WRITE
	ld	hl,#_kos_scratch
	rst	0x30
	jp	c,_exe_runtime_error
	ex	de,hl
	ret

; int diag_io_in(struct kos_diag_io_in_args *req);  HL = req.
_diag_io_in::
	ld	a,(hl)
	ld	(_kos_scratch),a
	ld	a,#SYS_DIAG_IO_IN
	ld	hl,#_kos_scratch
	rst	0x30
	jp	c,_exe_runtime_error
	ex	de,hl
	ret

; int diag_io_out(struct kos_diag_io_out_args *req);  HL = req.
_diag_io_out::
	ld	a,(hl)
	ld	(_kos_scratch),a
	inc	hl
	ld	a,(hl)
	ld	(_kos_scratch+1),a
	ld	a,#SYS_DIAG_IO_OUT
	ld	hl,#_kos_scratch
	rst	0x30
	jp	c,_exe_runtime_error
	ex	de,hl
	ret

; int diag_bank_info(struct kos_bank_info *out);  HL = out.
_diag_bank_info::
	ld	(_kos_scratch+8),hl
	ld	a,#SYS_DIAG_BANK_INFO
	ld	hl,#0x0000
	rst	0x30
	jp	c,_exe_runtime_error
	ld	de,(_kos_scratch+8)
	ld	a,l
	ld	(de),a			; bank
	inc	de
	ld	a,h
	ld	(de),a			; mask
	ld	de,#2
	ret

; int diag_system_info(struct kos_diag_system_info *out);  HL = out.
_diag_system_info::
	ld	(_kos_scratch+8),hl
	ld	a,#SYS_DIAG_SYSTEM_INFO
	ld	hl,#_kos_scratch
	rst	0x30
	jp	c,_exe_runtime_error
	ld	de,(_kos_scratch+8)
	ld	hl,#_kos_scratch
	ld	bc,#8
	ldir
	ld	de,#8
	ret
