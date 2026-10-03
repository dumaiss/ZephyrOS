; -----------------------------------------------------------------------------
; ZephyrOS M7 diagnostic port helpers (bank-7 code).
;
; The diagnostic I/O services in kernel/syscall/diag.c validate the port
; (10h-FFh; 00h-0Fh alias the memory latch and are rejected before this
; code runs) and then call these two leaf helpers.  They touch no bank,
; mode, stack, or interrupt state: the exact active latch is trivially
; unchanged.
;
;   struct zos_diag_hw_port { uint8_t port; uint8_t value; };
;   uint8_t zos_diag_hw_in(const struct zos_diag_hw_port *port);
;   void    zos_diag_hw_out(const struct zos_diag_hw_port *port);
;
; SDCC sdcccall(1): the single 16-bit pointer argument arrives in HL;
; the 8-bit read result is returned in A.
; -----------------------------------------------------------------------------

	.module diag_io

	.globl _zos_diag_hw_in
	.globl _zos_diag_hw_out

	.area _CODE

; uint8_t zos_diag_hw_in(const struct zos_diag_hw_port *port);
_zos_diag_hw_in::
	ld	c,(hl)			; port
	ld	b,#0x00			; 8-bit port -> 00pp
	in	a,(c)
	ret

; void zos_diag_hw_out(const struct zos_diag_hw_port *port);
_zos_diag_hw_out::
	ld	c,(hl)			; port
	inc	hl
	ld	a,(hl)			; value
	ld	b,#0x00			; 8-bit port -> 00pp
	out	(c),a
	ret
