; -----------------------------------------------------------------------------
; ZephyrOS libzos: shared runtime state (work/28c-m9-user-sdk).
;
; Owns the fixed low-window data area every wrapper marshals through and
; the carry-to-errno mapper the FS and diagnostic gates share.  zex.mk
; links _ZOS_SDK_DATA before executable code, so these blocks stay inside
; 0100h-1FFFh even when a program's ordinary _DATA follows a large image.
;
; The block is large enough for the largest argument block (the eight-byte
; exec/rename blocks) and for the getinfo record.
; -----------------------------------------------------------------------------

	.module zos_runtime

	.globl _zex_runtime_error
	.globl _zos_scratch
	.globl zos_scratch
	.globl _zos_ipc_raw_req
	.globl _zos_ipc_lookup_block
	.globl _zos_ipc_call_block

	.area _CODE

; int _zex_runtime_error(void);  Entry: A = native errno (carry set).
; Returns DE = -errno (SDCC 16-bit return convention).
_zex_runtime_error::
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
; fixes _ZOS_SDK_DATA at its independent low address, so it must not alter
; the normal _CODE -> _DATA placement order.
	.area _DATA

	.area _ZOS_SDK_DATA

; Gate-visible state.  The sizes are pinned by the public ABI and by
; tests/test_ipc_selftest.py: scratch 16, raw request 4, lookup block 4,
; call block 44.  This single 68-byte area is loaded as writable zeroes.
_zos_scratch::
zos_scratch::
	.ds	16
_zos_ipc_raw_req::
	.ds	4
_zos_ipc_lookup_block::
	.ds	4
_zos_ipc_call_block::
	.ds	44
