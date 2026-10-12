; -----------------------------------------------------------------------------
; ZephyrOS zlibc: SDCC unsigned arithmetic-helper entry points.
;
; SDCC emits calls to these two-underscore symbols for unsigned / and %;
; the implementations live in sdcc_arith.c under names SDCC keeps in
; _CODE.  Each stub is a single jump.
; -----------------------------------------------------------------------------

	.module zos_sdcc_arith_entries

	.globl __divuint
	.globl __moduint
	.globl __divulong
	.globl __modulong
	.globl _zos_divuint
	.globl _zos_moduint
	.globl _zos_divulong
	.globl _zos_modulong

	.area _CODE

__divuint::
	jp	_zos_divuint
__moduint::
	jp	_zos_moduint
__divulong::
	jp	_zos_divulong
__modulong::
	jp	_zos_modulong
