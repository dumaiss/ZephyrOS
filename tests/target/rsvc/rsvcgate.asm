; -----------------------------------------------------------------------------
; ZephyrOS resident-service spike: raw evaluation-syscall stub (test only).
;
; The evaluation numbers (40h-47h) are not part of the SDK, so the client
; program brings its own gate stub, modelled on user/sdk/src/fsgate.asm:
; the request { number, block } is built in the client's low-window
; scratch, this stub converts the private SDCC convention to the native
; RST 30h ABI and maps a carry-set nucleus error to -errno through the
; runtime's error path.  A negative *service* status is a normal result and
; returns unchanged.
;
;   int rsvc_raw_call(struct rsvc_raw_req *req);
;   struct rsvc_raw_req { unsigned char number; void *block; };
; -----------------------------------------------------------------------------

	.module rsvcgate

	.globl _rsvc_raw_call
	.globl _zex_runtime_error

	.area _CODE

_rsvc_raw_call::
	ld	a,(hl)			; syscall number
	inc	hl
	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	ex	de,hl			; HL = argument block
	rst	0x30
	jp	c,_zex_runtime_error
	ex	de,hl			; primary result -> DE
	ret
