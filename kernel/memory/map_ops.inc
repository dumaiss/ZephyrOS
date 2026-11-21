; -----------------------------------------------------------------------------
; KonaOS mapping operations: the Zephyr-80 Mk1 implementation.
;
; Every mapping change the nucleus and the boot path make goes through one
; of the operations below; no other kernel, boot or driver source names
; the memory latch (tests/test_source_layout.py enforces it).  A board with
; another mapper supplies its own implementation of the same operations.
;
; kernel/memory/mk2/map_ops.inc is the Zephyr-80 Mk2 implementation; a
; source that uses only operations both define assembles for either board,
; the include path choosing the file.
;
; On Mk1 a mapping value is the latch byte (architecture 2.1):
; (P << 5) | mode | Q.  The operations expand in place: no call, no stack,
; no memory access, so each runs on any stack, including none, and adds
; nothing to the caller's footprint.  They clobber only what is listed.
; An NMI handler never changes the mapping, so it never uses them.
;
; The including file includes hardware.inc first.
;
; Value operations (no mapping change):
;
;   KOS_MAP_CURRENT        A = the current mapping, exact.
;                          Flags kept.  2 bytes, 11 T.
;   KOS_MAP_BANK_OF        A = mapping -> A = its SRAM bank (the caller
;                          bank).  F.  2 bytes, 7 T.
;   KOS_MAP_IS_PROGRAM     A = mapping -> Z set when it names a program
;                          context (mode 10).  A, F.  4 bytes, 14 T.
;   KOS_MAP_NO_PAGE        A = mapping -> A = the same mapping with the
;                          window/aperture page closed (mode and bank
;                          kept).  F.  2 bytes, 7 T.
;   KOS_MAP_ROM_PAGE       A = ROM page 0-7 -> A = the mapping that reads
;                          that page (mode 00; SRAM bank 0 takes writes).
;                          F.  5 bytes, 19 T.
;
; Installing operations (each is one latch write):
;
;   KOS_MAP_RESTORE        install the mapping in A exactly: a saved
;                          KOS_MAP_CURRENT value.  Registers and flags
;                          kept.  2 bytes, 11 T.
;   KOS_MAP_SYSTEM         A = bank 0-6: the system mapping (mode 11,
;                          P = 0) with that bank in the caller window.
;                          A, F.  4 bytes, 18 T.
;   KOS_MAP_SYSTEM_AT b    the system mapping over the constant bank b.
;                          A.  4 bytes, 18 T.
;   KOS_MAP_SYSTEM_WINDOW  A = bank, B = window page field (page <<
;                          KOS_ROM_PAGE_SHIFT): the system mapping with
;                          that window page.  A, F.  5 bytes, 22 T.
;   KOS_MAP_APP            A = bank 0-6: that bank's application mapping
;                          (mode 10, P = 0).  A, F.  4 bytes, 18 T.
;   KOS_MAP_APP_AT b       the application mapping of the constant bank b.
;                          A.  4 bytes, 18 T.
;   KOS_MAP_WINDOW         A = window page 0-7, C = a KOS_MAP_NO_PAGE
;                          value: that mapping with the window showing
;                          the page.  A, F.  8 bytes, 34 T.
;   KOS_MAP_HOME           A = mapping: install it with the window/aperture
;                          page closed.  A, F.  4 bytes, 18 T.
;   KOS_MAP_CALLER_WINDOW s  A = mapping, s = a register or #bank: install
;                          it with bank s in the caller window (mode and
;                          page kept).  A, F.  5 bytes, 22 T (register);
;                          6 bytes, 25 T (#bank).
;   KOS_MAP_APERTURE       A = an aperture mapping (mode 10, caller bank,
;                          aperture page) composed by the IPC resolver:
;                          open it.  Registers and flags kept.  2 bytes,
;                          11 T.
;   KOS_MAP_ENTER_ROM      A = a KOS_MAP_ROM_PAGE value: enter the ROM
;                          view.  The next fetch comes from the ROM page,
;                          so the code after it is mirrored there.
;                          Registers and flags kept.  2 bytes, 11 T.
;   KOS_MAP_GATE_ENTER slot, fault
;                          the syscall gate's entry: slot = the caller's
;                          exact mapping; anything but mode 10 over bank
;                          0-6 jumps to fault; then the system mapping
;                          over the caller's bank (the caller window).
;                          A, F.  31 bytes, 123 T.
;   KOS_MAP_ROM_AT p,b     the ROM view of the constant page p with writes
;                          to the constant bank b (the reset copier and
;                          the warm reboot).  A.  4 bytes, 18 T.
; -----------------------------------------------------------------------------

	.macro	KOS_MAP_CURRENT
	in	a,(KOS_MEMCTL_PORT)
	.endm

	.macro	KOS_MAP_BANK_OF
	and	#KOS_BANK_MASK
	.endm

	.macro	KOS_MAP_IS_PROGRAM
	and	#KOS_MODE_MASK
	cp	#KOS_MODE_APPLICATION
	.endm

	.macro	KOS_MAP_NO_PAGE
	and	#(KOS_MODE_MASK | KOS_BANK_MASK)
	.endm

	.macro	KOS_MAP_ROM_PAGE
	rrca
	rrca
	rrca
	and	#KOS_ROM_PAGE_MASK
	.endm

	.macro	KOS_MAP_RESTORE
	out	(KOS_MEMCTL_PORT),a
	.endm

	.macro	KOS_MAP_SYSTEM
	or	#KOS_MODE_OS
	out	(KOS_MEMCTL_PORT),a
	.endm

	.macro	KOS_MAP_SYSTEM_AT bank
	ld	a,#(KOS_MODE_OS | (bank))
	out	(KOS_MEMCTL_PORT),a
	.endm

	.macro	KOS_MAP_SYSTEM_WINDOW
	or	#KOS_MODE_OS
	or	b
	out	(KOS_MEMCTL_PORT),a
	.endm

	.macro	KOS_MAP_APP
	or	#KOS_MODE_APPLICATION
	out	(KOS_MEMCTL_PORT),a
	.endm

	.macro	KOS_MAP_APP_AT bank
	ld	a,#(KOS_MODE_APPLICATION | (bank))
	out	(KOS_MEMCTL_PORT),a
	.endm

	.macro	KOS_MAP_WINDOW
	rrca
	rrca
	rrca
	and	#KOS_ROM_PAGE_MASK
	or	c
	out	(KOS_MEMCTL_PORT),a
	.endm

	.macro	KOS_MAP_HOME
	and	#(KOS_MODE_MASK | KOS_BANK_MASK)
	out	(KOS_MEMCTL_PORT),a
	.endm

	.macro	KOS_MAP_CALLER_WINDOW src
	and	#KOS_LATCH_NON_BANK_MASK
	or	src
	out	(KOS_MEMCTL_PORT),a
	.endm

	.macro	KOS_MAP_APERTURE
	out	(KOS_MEMCTL_PORT),a
	.endm

	.macro	KOS_MAP_ENTER_ROM
	out	(KOS_MEMCTL_PORT),a
	.endm

	.macro	KOS_MAP_GATE_ENTER slot, fault
	KOS_MAP_CURRENT
	ld	(slot),a
	KOS_MAP_IS_PROGRAM
	jp	nz,fault
	ld	a,(slot)
	KOS_MAP_BANK_OF
	cp	#KOS_BANK_MASK		; bank 7 is not an application bank
	jp	z,fault
	ld	a,(slot)
	KOS_MAP_BANK_OF
	KOS_MAP_SYSTEM
	.endm

	.macro	KOS_MAP_ROM_AT page, bank
	ld	a,#(((page) << KOS_ROM_PAGE_SHIFT) | KOS_MODE_BOOT | (bank))
	out	(KOS_MEMCTL_PORT),a
	.endm
