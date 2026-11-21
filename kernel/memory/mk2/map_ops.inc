; -----------------------------------------------------------------------------
; KonaOS mapping operations: the Zephyr-80 Mk2 implementation (the LS612
; DAT mapper, architecture 2.7).
;
; The Mk2 counterpart of kernel/memory/map_ops.inc: the same purposes under
; the same names where the purpose exists on this board, plus the DAT
; operations.  A source assembled for Mk2 finds this file through the Mk2
; include path; no other kernel source names a mapper port.
;
; The mapping value (the byte Mk1 keeps as the latch) names the context in
; force: 00h is the system bank; any other value is the working bank, and
; the value is the page of its logical page 0 - the program's page-0
; image, an SRAM page (40h-7Fh), which also names the program.  The
; working bank's full contents are tracked separately: KOS_DAT_CACHED is
; the 8-byte image it holds (memory.inc).
;
; NMI rule (architecture 2.7.6): every operation writes the mirror before
; the OUT, so at every instruction boundary the mirrors describe the
; mapping in force or the one the next OUT installs.  An NMI handler never
; uses these operations.  Entry writes are wiring-agnostic: OUT (n),A puts
; the page on D0-D6 and A8-A14.  The control register needs BC = 800Fh.
;
; The including file includes hardware.inc and memory.inc first.
;
; Value operations (no mapping change):
;
;   KOS_MAP_CURRENT        A = the current mapping value.  F, HL.
;                          9 bytes, 38 T.
;   KOS_MAP_BANK_OF        A = mapping -> A = the caller's page 0: the
;                          value itself.  0 bytes.
;   KOS_MAP_IS_PROGRAM     A = mapping -> Z set when it names a program
;                          context.  A, F.  4 bytes, 15 T.
;   KOS_MAP_IS_SYSTEM      A = mapping -> Z set when it is the system
;                          bank.  F.  1 byte, 4 T.
;   KOS_MAP_PROGRAM_CHECK fault
;                          A = a launch request's context byte (the
;                          child's page-0 page): jump to fault unless it
;                          names a program.  F.  4 bytes, 14 T.
;
; Installing operations:
;
;   KOS_MAP_RESTORE        install the mapping value in A exactly.
;                          Leaving to a program closes an open syscall
;                          crossing: the caller's working image comes
;                          back if a body replaced it, and system entry 0
;                          its own page.  Interrupts masked.  A, F.
;                          3 bytes; with kos_dat_restore 101 T to the
;                          system bank, 130 T to a program, 279 T closing
;                          a crossing (+ KOS_DAT_LOAD_WORKING when the
;                          body replaced the image).
;   KOS_MAP_HOME           install the mapping value in A: the Mk2 value
;                          has no window page, so this is KOS_MAP_RESTORE.
;   KOS_MAP_SYSTEM         select the system bank (A ignored).  A, BC.
;                          10 bytes, 42 T.
;   KOS_MAP_APP            enter the running program (A ignored): the
;                          working bank gets the running image
;                          (kos_dat_running) unless it holds it, then is
;                          selected.  System bank, interrupts masked.
;                          A, F.  3 bytes; kos_dat_enter 163 T when the
;                          bank holds the image (+ KOS_DAT_LOAD_WORKING).
;   KOS_MAP_GATE_ENTER slot, fault
;                          the syscall gate's entry: slot = the caller's
;                          mapping value; anything but a program context
;                          jumps to fault.  Saves system entry 0 and the
;                          working image's identity (copying the working
;                          mirror into the gate's image when an entry was
;                          remapped), opens the crossing, maps the caller's
;                          page 0 as system entry 0 and selects the system
;                          bank.  Interrupts masked.  A, F, BC, DE, HL.
;                          242 T (a remapped working bank: +214 T).
;   KOS_MAP_GATE_SUSPEND   the launch of a child from a syscall body at
;                          depth kos_proc_index: the open crossing's state
;                          (the gate's DAT block) is kept for that depth
;                          and the crossing closed for the child (system
;                          entry 0 back to the system page-0 image).
;                          Interrupts masked.  A, F, BC, DE, HL.  6 bytes
;                          + kos_dat_gate_suspend.
;   KOS_MAP_GATE_RESUME    the resume of depth kos_proc_index: its kept
;                          crossing is open again (kos_gate_saved_latch,
;                          the parent's page 0, as system entry 0); at
;                          depth 0 the exited child's crossing is closed.
;                          Interrupts masked.  A, F, BC, DE, HL.  6 bytes
;                          + kos_dat_gate_resume.
;
; DAT operations:
;
;   KOS_DAT_SELECT         A = control byte: install it.  BC.  8 bytes, 35 T.
;   KOS_DAT_SET n          A = page, n = entry 0-15 (constant): write
;                          one entry, mirror first.  The image identity
;                          is the caller's (kos_dat_load_working).
;                          5 bytes, 24 T.
;   KOS_DAT_REMAP n        A = page, n = entry 0-15 (constant): remap one
;                          entry.  0-7: KOS_DAT_SET.  8-15 also marks
;                          the working bank as holding no image first:
;                          HL; 10 bytes, 44 T.
;   KOS_DAT_SET_AT         A = page, C = system entry 0-7 (a register): write
;                          it, mirror first, OUT (C),A with B = A.  F, B,
;                          HL.  9 bytes, 52 T.
;   KOS_DAT_LOAD_WORKING   HL = an 8-byte image: load it into the working
;                          bank, which must not be selected.  Interrupts
;                          masked.  A, F.  3 bytes, kos_dat_load_working
;                          371 T.
; -----------------------------------------------------------------------------

; Defined by this board's operations: a source shared with the Mk1 board
; assembles its Mk1-only parts under .ifndef KOS_MAP_DAT.
KOS_MAP_DAT = 1

	.globl	kos_dat_restore
	.globl	kos_dat_load_working
	.globl	kos_gate_saved_page0
	.globl	kos_gate_saved_image
	.globl	kos_gate_open
	.globl	kos_gate_image
	.globl	kos_dat_enter
	.globl	kos_dat_gate_suspend
	.globl	kos_dat_gate_resume

	.macro	KOS_MAP_CURRENT
	ld	hl,#KOS_DAT_WORKING
	ld	a,(KOS_DAT_CONTROL)
	rra				; carry = DAT_BANK
	sbc	a,a			; FFh working, 00h system
	and	(hl)			; the working bank's page 0
	.endm

	.macro	KOS_MAP_BANK_OF
	.endm

	.macro	KOS_MAP_IS_PROGRAM
	add	a,#0xFF			; carry = (A != 0)
	sbc	a,a
	inc	a			; 0 (Z) for a program context
	.endm

	.macro	KOS_MAP_IS_SYSTEM
	or	a
	.endm

	.macro	KOS_MAP_PROGRAM_CHECK fault
	or	a
	jp	z,fault
	.endm

	.macro	KOS_MAP_RESTORE
	call	kos_dat_restore
	.endm

	.macro	KOS_MAP_HOME
	call	kos_dat_restore
	.endm

	.macro	KOS_DAT_SELECT
	ld	(KOS_DAT_CONTROL),a
	ld	bc,#KOS_DAT_CONTROL_PORT
	out	(c),a
	.endm

	.macro	KOS_MAP_SYSTEM
	ld	a,#KOS_DAT_CTL_SYSTEM
	KOS_DAT_SELECT
	.endm

	.macro	KOS_MAP_APP
	call	kos_dat_enter
	.endm

	.macro	KOS_DAT_SET n
	ld	(KOS_DAT_SYSTEM + (n)),a
	out	(KOS_DAT_SYSTEM_PORT + (n)),a
	.endm

	.macro	KOS_DAT_SET_AT
	.ifne	KOS_DAT_SYSTEM_PORT
	.error	1	; C is the entry and its port
	.endif
	ld	hl,#KOS_DAT_SYSTEM
	ld	b,#0
	add	hl,bc
	ld	(hl),a
	ld	b,a
	out	(c),a
	.endm

	.macro	KOS_DAT_REMAP n
	.ifge	(n) - KOS_DAT_ENTRIES
	ld	hl,#KOS_DAT_CACHED + 1
	ld	(hl),#0
	.endif
	KOS_DAT_SET n
	.endm

	.macro	KOS_DAT_LOAD_WORKING
	call	kos_dat_load_working
	.endm

	.macro	KOS_MAP_GATE_ENTER slot, fault, ?have
	KOS_MAP_CURRENT
	ld	(slot),a
	or	a			; the system bank made no call
	jp	z,fault
	ld	a,(KOS_DAT_SYSTEM)
	ld	(kos_gate_saved_page0),a
	ld	hl,(KOS_DAT_CACHED)
	ld	a,h
	or	a
	jr	nz,have
	; An entry was remapped: the gate keeps the exact bank as its image.
	ld	hl,#KOS_DAT_WORKING
	ld	de,#kos_gate_image
	ld	bc,#KOS_DAT_ENTRIES
	ldir
	ld	hl,#kos_gate_image
	ld	(KOS_DAT_CACHED),hl
have:
	ld	(kos_gate_saved_image),hl
	ld	a,#1
	ld	(kos_gate_open),a
	ld	a,(slot)
	KOS_DAT_REMAP 0
	KOS_MAP_SYSTEM
	.endm

	.macro	KOS_MAP_GATE_SUSPEND
	ld	a,(kos_proc_index)
	call	kos_dat_gate_suspend
	.endm

	.macro	KOS_MAP_GATE_RESUME
	ld	a,(kos_proc_index)
	call	kos_dat_gate_resume
	.endm
