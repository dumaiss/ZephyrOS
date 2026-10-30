; -----------------------------------------------------------------------------
; KonaOS libkos: C runtime startup.
;
; The launch gate enters here in mode 10 with HL = KOS_STARTUP_BASE, the
; startup block v1.  crt0 saves the block for startup(), copies the raw
; tail into program data (the kernel's page-zero copy is not preserved
; across a syscall), runs the documented splitter (quotes and escapes,
; docs/exe-format.md section 5) over that copy into a bounded argv
; (KOS_ARGV_MAX entries in BSS), calls int main(int argc, char **argv),
; and exits with the low 16 bits of the result.  A program never writes
; entry assembly.
;
; crt0 is linked first so it lands at 0100h (entry_offset 0) and its BSS
; (tail copy, argv) sits at the start of the data area.
; -----------------------------------------------------------------------------

	.module kos_crt0

	.globl kos_entry
	.globl _main
	.globl _splitargs
	.globl _exit
	.globl _kos_startup_block

	.area _CODE

kos_entry::
	ld	(_kos_startup_block),hl
	; Validate version and length; anything unexpected yields an empty
	; command line instead of reading past the block.
	ld	de,#_kos_tail_buffer
	ld	a,(hl)
	cp	#0x01
	jr	nz,003$
	inc	hl
	ld	a,(hl)
	cp	#126			; > KOS_STARTUP_TAIL_MAX (125)
	jr	nc,003$
	inc	hl
	; Copy tail_length bytes (B) from the block to the program buffer,
	; then NUL-terminate it: page zero is only valid until the first
	; syscall, and argv must survive the calls main() makes.
	ld	b,a
001$:
	ld	a,b
	or	a
	jr	z,002$
	ld	a,(hl)
	ld	(de),a
	inc	hl
	inc	de
	dec	b
	jr	001$
002$:
	xor	a
	ld	(de),a
	jr	004$
003$:
	xor	a			; empty command line
	ld	(de),a
004$:
	ld	hl,#_kos_tail_buffer
	ld	(_kos_tail),hl
	; splitargs(tail, argv, KOS_ARGV_MAX): the third argument is a stack
	; argument on the SDCC Z80 and the callee pops it.
	ld	hl,#16
	push	hl
	ld	hl,#_kos_tail_buffer
	ld	de,#_kos_argv
	call	_splitargs
	; DE = total word count; clamp argc to the stored capacity.
	ex	de,hl
	ld	a,h
	or	a
	jr	nz,005$
	ld	a,l
	cp	#16
	jr	c,006$
005$:
	ld	hl,#16
006$:
	ld	de,#_kos_argv
	call	_main
	; int main(...) returns the status in DE; SYS_EXIT takes HL.
	ex	de,hl
	call	_exit
	di
007$:
	halt
	jr	007$

; A program's constant data (its C sources compile with --constseg
; RCONST, mk/kos.mk) follows the code and precedes the BSS: in a placed
; program it is resident (architecture 7.2), so a pointer to it reaches
; it from every zone.
	.area _RCONST

	.area _DATA

; Bounded argv store, the tail copy and the tail pointer, zero-initialized
; by the EXE loader (the block is writable BSS, never an initializer).
_kos_tail::
	.ds	2
_kos_tail_buffer::
	.ds	127			; KOS_STARTUP_TAIL_MAX + 1
_kos_argv::
	.ds	32
