; -----------------------------------------------------------------------------
; KonaOS V9958 font bulk segment.
;
; The pinned CP850 6x8 bitmap font, assembled straight from the reviewable
; .inc asset into the driver image's bulk segment (_BULK, role `bulk`,
; placed in bank 6's boot/bulk tier 0000h-5FFFh).  The boot segment reads
; it directly (mode 10 maps the whole bank) and the run-time display
; reset re-reads it through the nucleus bulk copy, because the mode-11
; driver window cannot show that tier.
;
; tools/fontpack.py verifies the exact 2048 bytes and the pinned SHA-256
; for the ROMFS copy the tests use; this object is the resident form.
; -----------------------------------------------------------------------------

	.module v9958_font

	.area _BULK

	.globl _v9958_font_image
_v9958_font_image::
	.include "font_cp850_6x8.inc"

	.globl v9958_font_end
v9958_font_end::
