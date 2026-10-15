; -----------------------------------------------------------------------------
; KonaOS libkos: the device-event trampoline.
;
; The nucleus delivers an event by RETI-ing into this entry: the frame of the
; interrupted code is already stacked on the program's own stack, BC = the
; application handler, HL = count and DE = missed (sdcccall(1) argument
; positions).  The trampoline calls the handler through the SDK's indirect
; call helper and re-enters the nucleus with A = EVENT_RETURN.
;
; SYS_EVENT_RETURN never comes back: it either hands the next pending event
; over through the same frame or unwinds the frame to the interrupted code.
; The RST's own return address is discarded with the frame - the frame says
; where the program was - so this is a signal, not a call: the handler's
; return lands exactly at the interrupted instruction with the interrupted
; registers (the syscall's, when the event arrived at a syscall return).
;
; The trampoline is libkos code in the program's primary image, never in a
; far segment: the nucleus returns with the program's own bank selected and
; only the page field cleared (docs/sdk.md section 7).
;
; Every SDK call marshals through the same low-window argument blocks - they
; must lie in 0100h-1FFFh, so there is one shared area - and a delivery can
; land between the program's staging of a block and its RST, or between a
; syscall's return and the wrapper's reading of its reply.  The handler may
; itself call the SDK, so the trampoline saves that whole area around the
; handler and restores it: the interrupted call's staged block and its reply
; survive whatever the handler does.
; -----------------------------------------------------------------------------

	.module kos_event_trampoline
	.globl _kos_event_entry
	.globl ___sdcc_call_iy
	.globl s__KOS_SDK_DATA
	.globl l__KOS_SDK_DATA

SYS_EVENT_RETURN = 0x0E

	.area _CODE

; The SDCC indirect call through IY: exactly what the compiler emits for a
; call through a function pointer (the handler is in BC, the RETI argument).
_kos_event_entry::
	push	hl			; count, for the handler
	push	de			; missed
	push	bc			; the handler itself
	; Save the program's argument blocks (the LDIR needs all three
	; register pairs, and the handler call needs them all).
	ld	hl,#s__KOS_SDK_DATA
	ld	de,#_kos_event_block_save
	ld	bc,#l__KOS_SDK_DATA
	ldir
	pop	bc
	pop	de
	pop	hl
	push	bc
	pop	iy
	call	___sdcc_call_iy		; handler(count, missed)
	; Put the interrupted call's blocks back before it resumes: its
	; staged request, or the reply it is about to read.
	ld	hl,#_kos_event_block_save
	ld	de,#s__KOS_SDK_DATA
	ld	bc,#l__KOS_SDK_DATA
	ldir
	ld	a,#SYS_EVENT_RETURN
	rst	0x30
	; SYS_EVENT_RETURN never returns; trap a kernel contract violation.
	di
001$:
	halt
	jr	001$

; The save area is exactly the argument-block area (l__KOS_SDK_DATA bytes).
	.area _DATA
_kos_event_block_save::
	.ds	93
