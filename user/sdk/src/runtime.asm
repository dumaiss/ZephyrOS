; -----------------------------------------------------------------------------
; KonaOS libkos: shared runtime state.
;
; Owns the fixed low-window data area every wrapper marshals through and
; the carry-to-errno mapper the gates share.  exe.mk links _KOS_SDK_DATA
; before executable code, so these blocks stay inside 0100h-1FFFh even
; when a program's ordinary _DATA follows a large image.
;
; The block is large enough for the largest argument block (the 44-byte
; IPC call block) and for the getinfo record.
; -----------------------------------------------------------------------------

	.module kos_runtime

	.globl _exe_runtime_error
	.globl _kos_scratch
	.globl kos_scratch
	.globl _kos_ipc_raw_req
	.globl _kos_ipc_call_block

	.area _CODE

; int _exe_runtime_error(void);  Entry: A = native errno (carry set).
; Returns DE = -errno (SDCC 16-bit return convention).
_exe_runtime_error::
	ld	e,a
	ld	d,#0
	xor	a
	sub	e
	ld	e,a
	ld	a,#0
	sbc	a,d
	ld	d,a
	ret

; Declare ordinary BSS after code even in NO_CRT0 programs.  The linker
; fixes _KOS_SDK_DATA at its independent low address, so it must not alter
; the normal _CODE -> _DATA placement order.
	.area _DATA

	.area _KOS_SDK_DATA

; Gate-visible state.  The sizes are pinned by the public ABI and by
; tests/test_ipc_selftest.py: scratch 16, raw request 4, call block 44.
; The object and namespace blocks live beside these in nsgate.asm; the
; two areas together are the 93 bytes exe.mk places before executable
; code and are loaded as writable zeroes.
_kos_scratch::
kos_scratch::
	.ds	16
_kos_ipc_raw_req::
	.ds	4
_kos_ipc_call_block::
	.ds	44
