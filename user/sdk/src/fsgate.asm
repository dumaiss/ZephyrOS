; -----------------------------------------------------------------------------
; ZephyrOS libzos: filesystem/exec gate stub.
;
; Sibling of console.asm and diaggate.asm.  The FS wrappers build their
; packed argument block in the shared low-window scratch and call this
; stub with a {number, block} request; the stub converts the private SDCC
; convention to the native register ABI, runs RST 30h, and maps a
; carry-set error to -errno through runtime.asm.
; -----------------------------------------------------------------------------

	.module zos_fsgate

	.globl _zos_fs_call
	.globl _zex_runtime_error

	.area _CODE

; int zos_fs_call(struct zos_fs_call_req *req);  HL = req.
; Request layout (packed): +0 number, +1..+2 block pointer.
_zos_fs_call::
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
