; -----------------------------------------------------------------------------
; ZephyrOS native startup-block runtime: entry glue (user/runtime).
;
; The kernel enters every native program with HL pointing at its startup
; block v1 in page zero (0080h).  A program that wants its arguments has
; its entry glue call zos_args_init() before anything else; this routine
; saves HL and returns it unchanged, so the entry can continue with HL
; intact and C code can reach the block through zos_args().  Programs
; that ignore HL do not link this object and do not grow.
;
; SDCC name mapping: the C functions are zos_args_init()/zos_args(); the
; saved pointer is the C object zos_startup_block (zero-initialized, so
; zos_args() returns NULL until zos_args_init() runs).
; -----------------------------------------------------------------------------

	.module zex_args

	.globl _zos_args_init
	.globl _zos_args
	.globl _zos_startup_block

	.area _CODE

; void zos_args_init(void);  Entry: HL = startup block.  Preserves HL.
_zos_args_init::
	ld	(_zos_startup_block),hl
	ret

; struct zex_startup *zos_args(void);  The saved block, or NULL.
; SDCC returns a 16-bit value in DE.
_zos_args::
	ld	hl,(_zos_startup_block)
	ex	de,hl
	ret

	.area _DATA

; The saved startup-block pointer (2 bytes), zero-initialized by the ZEX
; zero-BSS load, so zos_args() reports "not initialized" until the glue
; runs.  It is reserved (`.ds`), never an initializer: ZEX v1 supports no
; initialized writable data.
_zos_startup_block::
	.ds	2
