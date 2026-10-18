; -----------------------------------------------------------------------------
; KonaOS libkos: wait for the next interrupt.
;
; void kos_idle_halt(void);  (idle.c decides when it is safe)
;   HALT until an interrupt arrives, when interrupts are enabled.  With
;   interrupts masked it returns at once (a HALT then would never end).
;
;   IFF2 is read up to three times: on the board an interrupt accepted
;   right after LD A,I makes that read report 0, and the interrupt's own
;   RETI makes the next read true.  Clobbers AF.
; -----------------------------------------------------------------------------

	.module idle_halt
	.globl _kos_idle_halt

	.area _CODE

_kos_idle_halt::
	ld	a,i
	jp	pe,idle_halt
	ld	a,i
	jp	pe,idle_halt
	ld	a,i
	ret	po
idle_halt:
	halt
	ret
