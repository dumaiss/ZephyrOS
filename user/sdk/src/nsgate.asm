; -----------------------------------------------------------------------------
; KonaOS libkos: object and namespace gate blocks (packages 42a, 42b).
;
; The bind, unbind, ns_enum, chdir and getcwd wrappers marshal through
; these gate-visible blocks; every one lives in the single _KOS_SDK_DATA
; area, which exe.mk links before executable code so the blocks stay
; inside 0100h-1FFFh (docs/sdk.md).  The wrapper builds the block and
; calls the shared raw gate stub (ipcgate.asm / gate.asm).
;
; The sizes are pinned by abi/kos/object.h and by tests/test_ns_selftest.py
; and tests/test_ipc_selftest.py: bind 8, unbind 1, ns_enum 12, chdir 4,
; getcwd 4.
; -----------------------------------------------------------------------------

	.module nsgate

	.globl _kos_bind_block
	.globl _kos_unbind_block
	.globl _kos_ns_enum_block
	.globl _kos_chdir_block
	.globl _kos_getcwd_block
	.globl _kos_ipc_raw_call

	.area _KOS_SDK_DATA

_kos_bind_block::
	.ds	8
_kos_unbind_block::
	.ds	1
_kos_ns_enum_block::
	.ds	12
_kos_chdir_block::
	.ds	4
_kos_getcwd_block::
	.ds	4
