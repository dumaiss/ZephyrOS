; -----------------------------------------------------------------------------
; KonaOS libkos: wait for the next interrupt.
;
; void kos_idle_halt(volatile uint8_t *last);  (idle.c decides when it is
; safe; HL = the nucleus's last-source byte, KOS_IRQ_LAST_SOURCE)
;   HALT until a maskable interrupt arrives, when interrupts are enabled.
;   With interrupts masked it returns at once (a HALT then would never
;   end).  An NMI (the RT clock's tick) also ends a HALT, but only a
;   maskable interrupt's entry writes the last-source byte: the byte is set
;   to KOS_IRQ_NO_SOURCE first, and a wake that leaves it so halts again.
;
;   IFF2 is read up to three times: on the board an interrupt accepted
;   right after LD A,I makes that read report 0, and the interrupt's own
;   RETI makes the next read true.  Clobbers AF.
; -----------------------------------------------------------------------------

	.module idle_halt
	.globl _kos_idle_halt

	.area _CODE

IDLE_NO_SOURCE = 0xFF                   ; KOS_IRQ_NO_SOURCE

_kos_idle_halt::
	ld	a,i
	jp	pe,idle_halt
	ld	a,i
	jp	pe,idle_halt
	ld	a,i
	ret	po
idle_halt:
	; An interrupt between the store and the HALT only makes this wait
	; run to the next one.
	ld	(hl),#IDLE_NO_SOURCE
idle_wait:
	halt
	ld	a,(hl)
	inc	a
	jr	z,idle_wait		; an NMI's wake: no interrupt yet
	ret
