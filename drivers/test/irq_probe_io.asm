; -----------------------------------------------------------------------------
; Stack-pointer probe for the M3 test-only IRQ driver.
;
; The registered CTC0 handler uses this to record that it executed on the
; bank-7 ISR stack, exactly like the M2 acceptance evidence.  SDCC
; sdcccall(1): 16-bit result in DE.
; -----------------------------------------------------------------------------

	.module irq_probe_io
	.globl _zos_m3_probe_sp

	.area _CODE

; uint16_t zos_m3_probe_sp(void);
_zos_m3_probe_sp::
	ld	hl,#0x0000
	add	hl,sp
	ex	de,hl
	ret
