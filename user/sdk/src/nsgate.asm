; -----------------------------------------------------------------------------
; ZephyrOS libzos: object and namespace gate blocks (packages 42a, 42b).
;
; The bind, unbind, ns_enum, chdir and getcwd wrappers marshal through
; these gate-visible blocks; every one lives in the single _ZOS_SDK_DATA
; area, which zex.mk links before executable code so the blocks stay
; inside 0100h-1FFFh (docs/sdk.md).  The wrapper builds the block and
; calls the shared raw gate stub (ipcgate.asm / gate.asm).
;
; The sizes are pinned by abi/zos/object.h and by tests/test_ns_selftest.py
; and tests/test_ipc_selftest.py: bind 8, unbind 1, ns_enum 12, chdir 4,
; getcwd 4.
; -----------------------------------------------------------------------------

	.module nsgate

	.globl _zos_bind_block
	.globl _zos_unbind_block
	.globl _zos_ns_enum_block
	.globl _zos_chdir_block
	.globl _zos_getcwd_block
	.globl _zos_ipc_raw_call

	.area _ZOS_SDK_DATA

_zos_bind_block::
	.ds	8
_zos_unbind_block::
	.ds	1
_zos_ns_enum_block::
	.ds	12
_zos_chdir_block::
	.ds	4
_zos_getcwd_block::
	.ds	4
