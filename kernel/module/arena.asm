; -----------------------------------------------------------------------------
; M4 driver-arena bound symbols.
;
; The arena's numeric geometry lives in platform/zephyr80/memory.inc (a
; declarative platform fact).  This file only names the bounds as link
; symbols so the loader can compute granule placement without hard-coding
; addresses.  The absolute areas contain no loadable bytes and are never
; copied to ROM/SRAM.
;
; __driver_arena_start  first byte of the arena (A000h, inclusive)
; __driver_arena_end    first byte after the arena (C000h, exclusive)
;
; SDCC prefixes C identifiers with one underscore, so C code declaring
; `extern uint8_t __driver_arena_start[]` resolves to the alias symbol
; ___driver_arena_start; both names sit at the same address.
; -----------------------------------------------------------------------------

	.module arena
	.globl __driver_arena_start, __driver_arena_end
	.globl ___driver_arena_start, ___driver_arena_end

	.include "zephyr80/memory.inc"

	.area _ARENA_DECL (ABS)
	.org ZOS_DRIVER_ARENA_BASE
__driver_arena_start::
___driver_arena_start::
	.org ZOS_DRIVER_ARENA_END
__driver_arena_end::
___driver_arena_end::
