; -----------------------------------------------------------------------------
; ZephyrOS libzos: shared runtime state (work/28c-m9-user-sdk).
;
; Owns the one low-window scratch block every wrapper marshals through and
; the carry-to-errno mapper the FS and diagnostic gates share.  The
; standard ZEX link layout places _DATA at the image end (inside the
; syscall argument window 0100h-1FFFh for every program the SDK builds);
; the manifest-checked symbol `zos_scratch` proves it stays there.
;
; The block is large enough for the largest argument block (the eight-byte
; exec/rename blocks) and for the getinfo record.
; -----------------------------------------------------------------------------

	.module zos_runtime

	.globl _zex_runtime_error
	.globl _zos_scratch
	.globl zos_scratch

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

	.area _DATA

; The shared low-window syscall argument block.  Both the C-mangling form
; and the plain name are exported so the generated manifest can pin the
; readable name.
_zos_scratch::
zos_scratch::
	.ds	16
