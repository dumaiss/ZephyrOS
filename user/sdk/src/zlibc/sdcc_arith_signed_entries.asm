; -----------------------------------------------------------------------------
; ZephyrOS zlibc: SDCC signed arithmetic-helper entry points.
;
; The signed counterparts of sdcc_arith.asm; implementations live in
; sdcc_arith_signed.c.
; -----------------------------------------------------------------------------

	.module zos_sdcc_arith_signed_entries

	.globl __divsint
	.globl __modsint
	.globl __divslong
	.globl __modslong
	.globl _zos_divsint
	.globl _zos_modsint
	.globl _zos_divslong
	.globl _zos_modslong

	.area _CODE

__divsint::
	jp	_zos_divsint
__modsint::
	jp	_zos_modsint
__divslong::
	jp	_zos_divslong
__modslong::
	jp	_zos_modslong
