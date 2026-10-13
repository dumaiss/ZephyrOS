; -----------------------------------------------------------------------------
; KonaOS diagnostic port helpers (cold-half code).
;
; The diagnostic I/O drivers in kernel/syscall/diag.c validate the port
; (10h-FFh; 00h-0Fh alias the memory latch and are rejected before this
; code runs) and then call these two leaf helpers.  They touch no bank,
; mode, stack, or interrupt state: the exact active latch is trivially
; unchanged.  The diagnostic syscall bodies that reach them run with
; P = 0 (the syscall gate clears the page field), so the helpers live in
; the cold half with the rest of the syscall side (config/residency.json).
;
;   struct kos_diag_hw_port { uint8_t port; uint8_t value; };
;   uint8_t kos_diag_hw_in(const struct kos_diag_hw_port *port);
;   void    kos_diag_hw_out(const struct kos_diag_hw_port *port);
;
; SDCC sdcccall(1): the single 16-bit pointer argument arrives in HL;
; the 8-bit read result is returned in A.
; -----------------------------------------------------------------------------

	.module diag_io

	.globl _kos_diag_hw_in
	.globl _kos_diag_hw_out

	.area _WINDOW

; uint8_t kos_diag_hw_in(const struct kos_diag_hw_port *port);
_kos_diag_hw_in::
	ld	c,(hl)			; port
	ld	b,#0x00			; 8-bit port -> 00pp
	in	a,(c)
	ret

; void kos_diag_hw_out(const struct kos_diag_hw_port *port);
_kos_diag_hw_out::
	ld	c,(hl)			; port
	inc	hl
	ld	a,(hl)			; value
	ld	b,#0x00			; 8-bit port -> 00pp
	out	(c),a
	ret
