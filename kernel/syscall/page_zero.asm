; -----------------------------------------------------------------------------
; KonaOS native page zero: RST 30h syscall vector.
;
; The vector is exactly three bytes: JP kos_syscall_entry.  The dispatcher
; does not live in page zero; kos_syscall_entry is the common-memory gate
; in kernel/syscall/entry.asm.  tools/pack_image.py verifies the
; exact address, size, and target of this stub at build time.
;
; KonaOS owns page zero 0000h-00FFh for native execution; the reset
; copier (0000h-0026h) is disjoint from this vector.
; -----------------------------------------------------------------------------

	.module page_zero
	.globl kos_syscall_entry

	.area _PAGE_ZERO (ABS)
	.org 0x0030

_kos_rst30_vector::
	jp	kos_syscall_entry
_kos_rst30_end::

	.ifgt (_kos_rst30_end - _kos_rst30_vector) - 3
	.error 1	; RST 30h vector must be exactly three bytes
	.endif
