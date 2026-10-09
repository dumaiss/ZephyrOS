; -----------------------------------------------------------------------------
; ZephyrOS libzos: object and namespace gate blocks (work package 42a).
;
; Sibling of runtime.asm's stage-3 blocks, linked only by the namespace
; acceptance image (ZOS_NS_SELFTEST): the SDK variant it builds
; (ZOS_NS_HANDLES) marshals bind, unbind and ns_enum through these three
; gate-visible argument blocks instead of the lookup block, so the low-data
; area grows by 21 bytes and every program of that image links its code
; above it (ZEX_DEFAULT_CODEBASE in user/sdk/zex.mk).
;
; The sizes are pinned by abi/zos/object.h and by tests/test_ns_selftest.py:
; bind 8, unbind 1, ns_enum 12.  Like the stage-3 blocks they live in the
; single _ZOS_SDK_DATA area, which is loaded as writable zeroes.
; -----------------------------------------------------------------------------

	.module nsgate

	.globl _zos_bind_block
	.globl _zos_unbind_block
	.globl _zos_ns_enum_block

	.area _ZOS_SDK_DATA

_zos_bind_block::
	.ds	8
_zos_unbind_block::
	.ds	1
_zos_ns_enum_block::
	.ds	12
