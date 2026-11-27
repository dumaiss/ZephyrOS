; -----------------------------------------------------------------------------
; The Mk1 syscall gate on the transitions test's harness (test-only;
; tests/test_mk2_transitions.py measures both boards' gates with it).
;
; Links the kernel's transition sources assembled for Mk1 (page zero, the
; syscall gate, its common state, the IRQ executive, the event core) with a
; null dispatcher and the stubs they reference.  The test splits the linked
; 64 KiB into bank 0 (0000h-1FFFh, the caller window and page zero;
; E000h-FFFFh, common) and bank 7 (2000h-DFFFh, the nucleus) and runs
; m1_call (the caller window, 0100h) in mode 10 over bank 0.
; -----------------------------------------------------------------------------

	.module	mk1gate

	.area	_CODE
	.area	_DATA
	.area	_PROC
	.area	_COMMON
	.area	_COMMON_DATA

	.area	_PROC
m1_call::
	rst	0x30
m1_return::
	jr	m1_return

	.area	_CODE
_kernel_syscall_dispatch::
	ld	hl,#0
	ld	(_kos_syscall_result_hl),hl
	xor	a
	ld	(_kos_syscall_result_a),a
	ld	(_kos_syscall_result_c),a
	ret
_kernel_main::
_panic::
	halt
_early_console_init::
_irq_dispatch::
	ret

	.area	_DATA
_kos_syscall_number::
	.ds	1
_kos_syscall_args::
	.ds	2
_kos_syscall_result_a::
	.ds	1
_kos_syscall_result_c::
	.ds	1
_kos_syscall_result_hl::
	.ds	2
