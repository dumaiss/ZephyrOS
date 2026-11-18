; -----------------------------------------------------------------------------
; The RT outcome program's handler (tests/rt/rt.c): counts its ticks in a
; word of the RT region.  Position independent - the program copies it to
; the region's base - and within the ABI's handler contract: it preserves
; HL (two bytes of stack), changes no mapping and returns with RETN.
; -----------------------------------------------------------------------------

	.module rt_handler
	.globl _rt_handler_image, _rt_handler_image_end

RT_COUNT = 0xF100               ; keep in step with rt.c's RT_COUNT

	.area _CODE

_rt_handler_image::
	push	hl
	ld	hl,(RT_COUNT)
	inc	hl
	ld	(RT_COUNT),hl
	pop	hl
	retn
_rt_handler_image_end::
