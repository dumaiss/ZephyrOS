; -----------------------------------------------------------------------------
; KonaOS libkos: aperture latch helpers.
;
; Opening the mode-10 far aperture is not a syscall: the program writes
; the token SYS_XMEM_MAP returned to the memory-control port and keeps the
; previous latch to restore.  These two entries are the register-only
; primitives; everything else about the aperture is ordinary code and
; ordinary pointer accesses (architecture 7.2).
;
; Port 00h is the banking latch on the Zephyr-80 (the module build never
; sees platform headers, so the constant is local and pinned here).
; -----------------------------------------------------------------------------

	.module kos_xmemgate

	.globl _aperture_open
	.globl _aperture_close

KOS_MEMCTL_PORT = 0x00

	.area _CODE

; unsigned char aperture_open(unsigned char token);  A = token.
; Writes the token and returns the previous latch in A.
_aperture_open::
	ld	c,a
	in	a,(KOS_MEMCTL_PORT)
	ld	b,a
	ld	a,c
	out	(KOS_MEMCTL_PORT),a
	ld	a,b
	ret

; void aperture_close(unsigned char previous);  A = the latch to restore.
_aperture_close::
	out	(KOS_MEMCTL_PORT),a
	ret
