; -----------------------------------------------------------------------------
; Bank-7 driver-arena bound symbols.
;
; There is one geometry.  The two-pass build measures, in a probe link, the
; exclusive end of every loadable bank-7 area (_CODE, _HOME, _INITIALIZER,
; _GSINIT, _GSFINAL) and the runtime-data size (l__DATA + l__INITIALIZED);
; tools/mapcheck.py `geometry derive` aligns the kernel end up to the
; 512-byte granule, derives the data base as
; align_down(DF00h - data_size, 512), and writes the results into the
; generated include `arena_geometry.inc`.  The probe link itself places no
; arena, so kernel growth can never collide with a placeholder.
;
; The generated file lives under the build directory and is included via
; the per-build include path; platform/zephyr80/memory.inc stays
; declarative.
;
; Exported symbols (the declaration area contains no bytes; zero-size ABS
; labels only):
;   __kernel_code_end     exclusive end of the linked loadable bank-7 areas
;   __driver_arena_start  first byte of the arena, granule aligned
;   __driver_arena_end    first byte after the arena (the derived data base)
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

	.if ZOS_ARENA_PROBE
; Probe link: place no arena.  The loader still references the exported
; bounds, so define them as zero absolute symbols; the derive step reads
; the loadable area ends from this map, never these values.
__driver_arena_start = 0x0000
___driver_arena_start = 0x0000
__driver_arena_end = 0x0000
___driver_arena_end = 0x0000
	.else
	.globl __kernel_code_end, ___kernel_code_end

	.org ZOS_KERNEL_CODE_END
__kernel_code_end::
___kernel_code_end::

	.org ZOS_DRIVER_ARENA_BASE
__driver_arena_start::
___driver_arena_start::

	.org ZOS_DRIVER_ARENA_END
__driver_arena_end::
___driver_arena_end::
	.endif
