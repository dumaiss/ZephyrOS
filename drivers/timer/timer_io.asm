; -----------------------------------------------------------------------------
; TIMER driver interrupt tokens (drivers/timer, MOD module).
;
; The clock's reading (`timer_counts_now` in timer.c) has to run with
; interrupts open: the CTC counts whether or not the CPU listens, so a
; masked reading can pair a stale tick count with a counter that has
; already rolled over, and the retry loop cannot see a tick that never
; interrupts.  Since the syscall gate hands a driver body the *caller's*
; interrupt state (correction 01), the reading must not assume either
; state: it saves what it found, opens for the read, and puts the saved
; state back - never forcing the caller's state on the way out.
;
; Standard Z80 pattern: LD A,I exposes IFF2 in P/V.
;
; Return values (sdcccall(1): 8-bit result in A):
;   timer_irq_save_enable  the previous IFF2 state (1 = enabled)
; -----------------------------------------------------------------------------

	.module timer_io
	.include "hardware.inc"

	.globl _timer_irq_save_enable
	.globl _timer_irq_restore

	.area _CODE

; ---------------------------------------------------------------------------
; uint8_t timer_irq_save_enable(void);  A = L = 1 if IFF2 was set.
; Snapshot the state, then open interrupts for the caller's critical read.
; ---------------------------------------------------------------------------
_timer_irq_save_enable::
	ld	a,i
	ei
	ld	l,#0
	jp	po,timer_irq_was_off
	inc	l
timer_irq_was_off:
	ld	a,l
	ret

; ---------------------------------------------------------------------------
; void timer_irq_restore(uint8_t token);  A = token: 1 re-enables, 0 masks.
; ---------------------------------------------------------------------------
_timer_irq_restore::
	or	a
	jp	nz,timer_irq_enable
	di
	ret
timer_irq_enable:
	ei
	ret
