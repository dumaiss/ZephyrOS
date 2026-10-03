; -----------------------------------------------------------------------------
; M8 driver-arena bound symbols.
;
; Production derives the arena from the linked kernel: the two-pass build
; measures the exclusive end of every loadable bank-7 area (_CODE, _HOME,
; _INITIALIZER, _GSINIT, _GSFINAL) in a probe link, aligns it up to the
; 512-byte granule, and writes the result into the generated include
; `arena_geometry.inc` (tools/mapcheck.py, `geometry derive`).  Pre-M8
; acceptance/variant images keep the fixed platform geometry; the same
; generated file carries those values with ZOS_ARENA_DERIVED = 0.
;
; The generated file lives under the build directory and is included via
; the per-build include path; platform/zephyr80/memory.inc stays
; declarative.
;
; Exported symbols (the declaration area contains no bytes; zero-size ABS
; labels only):
;   __kernel_code_end     exclusive end of the linked loadable bank-7 areas
;                         (derived builds only; absent in legacy builds)
;   __driver_arena_start  first byte of the arena, granule aligned
;   __driver_arena_end    first byte after the arena (C000h for production)
;
; SDCC prefixes C identifiers with one underscore, so C code declaring
; `extern uint8_t __driver_arena_start[]` resolves to the alias symbol
; ___driver_arena_start; both names sit at the same address.
; -----------------------------------------------------------------------------

	.include "arena_geometry.inc"

	.module arena
	.globl __driver_arena_start, __driver_arena_end
	.globl ___driver_arena_start, ___driver_arena_end

	.area _ARENA_DECL (ABS)

	.if ZOS_ARENA_DERIVED
	.globl __kernel_code_end, ___kernel_code_end
	.org ZOS_KERNEL_CODE_END
__kernel_code_end::
___kernel_code_end::
	.endif

	.org ZOS_DRIVER_ARENA_BASE
__driver_arena_start::
___driver_arena_start::

	.org ZOS_DRIVER_ARENA_END
__driver_arena_end::
___driver_arena_end::
