; -----------------------------------------------------------------------------
; KonaOS klibc: SDCC unsigned arithmetic-helper entry points.
;
; SDCC emits calls to these two-underscore symbols for unsigned / and %;
; the implementations live in sdcc_arith.c under names SDCC keeps in
; _CODE.  Each stub is a single jump.
; -----------------------------------------------------------------------------

	.module kos_sdcc_arith_entries

	.globl __divuint
	.globl __moduint
	.globl __divulong
	.globl __modulong
	.globl _kos_divuint
	.globl _kos_moduint
	.globl _kos_divulong
	.globl _kos_modulong

	.area _CODE

__divuint::
	jp	_kos_divuint
__moduint::
	jp	_kos_moduint
__divulong::
	jp	_kos_divulong
__modulong::
	jp	_kos_modulong
