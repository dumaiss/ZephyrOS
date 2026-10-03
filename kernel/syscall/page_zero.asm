; -----------------------------------------------------------------------------
; ZephyrOS native page zero: RST 30h syscall vector.
;
; The vector is exactly three bytes: JP zos_syscall_entry.  The dispatcher
; does not live in page zero; zos_syscall_entry is the common-memory gate
; in kernel/syscall/entry.asm.  tools/pack_image.py verifies the
; exact address, size, and target of this stub at build time.
;
; ZephyrOS owns page zero 0000h-00FFh for native execution; the reset
; copier (0000h-0026h) is disjoint from this vector.
; -----------------------------------------------------------------------------

	.module page_zero
	.globl zos_syscall_entry

	.area _PAGE_ZERO (ABS)
	.org 0x0030

_zos_rst30_vector::
	jp	zos_syscall_entry
_zos_rst30_end::

	.ifgt (_zos_rst30_end - _zos_rst30_vector) - 3
	.error 1	; RST 30h vector must be exactly three bytes
	.endif
