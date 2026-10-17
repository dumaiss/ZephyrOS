; -----------------------------------------------------------------------------
; KonaOS klibc: SDCC 32-bit unsigned arithmetic-helper entry points (the
; 16-bit pair is sdcc_arith16.asm).
;
; SDCC emits calls to these two-underscore symbols for unsigned / and %;
; the implementations live in sdcc_arith.c under names SDCC keeps in
; _CODE.  Each stub is a single jump.
; -----------------------------------------------------------------------------

	.module kos_sdcc_arith_entries

	.globl __divulong
	.globl __modulong
	.globl _kos_divulong
	.globl _kos_modulong

	.area _CODE

__divulong::
	jp	_kos_divulong
__modulong::
	jp	_kos_modulong
