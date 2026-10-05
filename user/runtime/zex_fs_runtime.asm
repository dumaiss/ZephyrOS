; -----------------------------------------------------------------------------
; ZephyrOS M9 native filesystem runtime: gate stub (user/runtime).
;
; Sibling of zex_runtime.asm / zex_diag_runtime.asm.  Native filesystem
; wrappers build their packed argument block in this module's low-window
; scratch (`zos_fs_scratch`, an _DATA object the standard ZEX link layout
; places at the start of BSS, inside 0100h-1FFFh) and call the stub with a
; {number, block} request; the stub converts the private SDCC convention to
; the native register ABI, runs RST 30h, and maps a carry-set error to
; -errno through the shared mapper in zex_runtime.asm.
;
; The syscall argument block is always the scratch block, never a program
; stack object near E000h, and bank 7 / window validation stay the kernel's
; concern.
;
; System call numbers (native ABI, fixed by M9):
;   10 OPEN   11 CLOSE  12 READ   13 WRITE  14 SEEK  15 STAT
;   16 OPENDIR 17 READDIR 18 CLOSEDIR 19 MKDIR 1A RMDIR 1B UNLINK
;   1C RENAME 1D CHDIR  1E GETCWD 1F SYNC
; -----------------------------------------------------------------------------

	.module zex_fs_runtime

	.globl _zos_fs_call
	.globl _zos_fs_scratch
	.globl zos_fs_scratch
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

	.area _DATA

; The shared argument block (16 bytes covers the largest block, the
; four-pointer rename block).  The manifest-checked symbol
; `zos_fs_scratch` proves it stays inside the syscall window; both the
; C-mangling form and the readable name are exported for the map check.
_zos_fs_scratch::
zos_fs_scratch::
	.ds	16
