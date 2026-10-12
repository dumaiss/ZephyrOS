; -----------------------------------------------------------------------------
; KonaOS klibc: SDCC signed arithmetic-helper entry points.
;
; The signed counterparts of sdcc_arith.asm; implementations live in
; sdcc_arith_signed.c.
; -----------------------------------------------------------------------------

	.module kos_sdcc_arith_signed_entries

	.globl __divsint
	.globl __modsint
	.globl __divslong
	.globl __modslong
	.globl _kos_divsint
	.globl _kos_modsint
	.globl _kos_divslong
	.globl _kos_modslong

	.area _CODE

__divsint::
	jp	_kos_divsint
__modsint::
	jp	_kos_modsint
__divslong::
	jp	_kos_divslong
__modslong::
	jp	_kos_modslong
