; -----------------------------------------------------------------------------
; KonaOS libkos: IPC gate stub (work package 39).
;
; Sibling of console.asm, fsgate.asm and diaggate.asm.  The IPC wrappers
; build their packed argument block (and the caller's frame) in the
; program's low window and call this stub with a {number, block, error}
; request; the stub converts the private SDCC convention to the native
; register ABI, runs RST 30h, records whether the nucleus returned a
; carry-set error (so the caller can tell a structural refusal from an
; IPC status carried in HL) and maps that error to -errno through
; runtime.asm.
; -----------------------------------------------------------------------------

	.module kos_ipcgate

	.globl _kos_ipc_raw_call
	.globl _exe_runtime_error

	.area _CODE

; int kos_ipc_raw_call(struct kos_ipc_call_req *req);  HL = req.
; Request layout (packed): +0 number, +1..+2 block pointer, +3 error flag.
; Returns the primary result in DE (the gate publishes it in HL) or, with
; the carried error recorded in the request, -errno.
_kos_ipc_raw_call::
	ld	a,(hl)			; syscall number
	inc	hl
	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	inc	hl			; HL -> error flag
	ld	(hl),#0
	push	hl			; keep &error across the syscall
	ex	de,hl			; HL = argument block
	rst	0x30
	jp	nc,001$
	; Carry set: the request never reached the IPC layer.  Record it and
	; return -errno (the ABI error code is in A).
	pop	hl
	ld	(hl),#1
	jp	_exe_runtime_error
001$:
	pop	de			; discard &error (DE = &error)
	ex	de,hl			; DE = primary result, HL = &error
	ret
