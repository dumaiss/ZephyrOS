; -----------------------------------------------------------------------------
; KonaOS resident-driver boot gate (architecture 5.2).
;
; A driver's boot segment lives in bank 6's boot/bulk tier
; (0000h-5FFFh), which the mode-11 window can never show, so the nucleus
; runs it once through the mode-10 gate with Q = 6: bank 6 answers
; 0000h-DFFFh and the permanent common region still answers
; E000h-FFFFh.  Every instruction here, the continuation after the latch
; write and the boot state therefore live in permanent common memory; the
; boot entry itself runs on the caller's stack, which the loader keeps in
; common memory (the kernel boot stack), so it stays visible across the
; mode change.
;
; Boot-tier code executes with bank 7 fully unmapped: it may touch its own
; bank, the common region and I/O ports, and it may call common-resident
; nucleus primitives, but it can reach neither the fixed core nor the cold
; half.  Nothing in the boot tier is executed after boot.
;
;   uint8_t kos_driver_run_boot(uint16_t entry);
;       HL = entry, must lie in 0000h-5FFFh (bank 6 boot/bulk tier)
;       returns 0, or KOS_EINVAL when the entry is outside the tier
;
; SDCC sdcccall(1): the 16-bit argument arrives in HL, the 8-bit result is
; returned in A.  Interrupts are masked for the mode change and the
; caller's exact interrupt state is restored; a boot segment that must
; wait on an interrupt enables them itself.  Single-depth: not reentrant
; (single-tasking, like the other common transition gates).
;
; The upcall request block below is also the loader's: the fixed upcall
; shim (kernel/memory/upcall.asm) reads and writes its request block with
; bank 7 mapped, so the block must not live in the cold half; these seven
; bytes are the loader's single fixed-core request block.
; -----------------------------------------------------------------------------

	.module driver_boot
	.include "hardware.inc"
	.include "memory.inc"

	.globl _kos_driver_run_boot
	.globl kos_driver_boot_entry
	.globl kos_driver_boot_saved_latch
	.globl kos_driver_boot_saved_iff
	.globl _kos_driver_upcall_block

; Bank 6 is the resident driver domain (revision 13: bank = KOS_BANK_MASK
; - window page), and the boot/bulk tier is 0000h-5FFFh.
KOS_DRIVER_BANK = KOS_BANK_MASK - 1
KOS_BOOT_TIER_END = 0x6000

	.area _COMMON

; ---------------------------------------------------------------------------
; _kos_driver_run_boot - C name kos_driver_run_boot.  Entry: HL = entry.
; ---------------------------------------------------------------------------
_kos_driver_run_boot::
	; Validate the entry against the boot/bulk tier while bank 7 is
	; still fully visible.
	ld	a,h
	cp	#(KOS_BOOT_TIER_END >> 8)
	jr	c,001$
	ld	a,#0x02			; KOS_EINVAL: not in the boot tier
	ret
001$:
	ld	(kos_driver_boot_entry),hl

	; Snapshot IFF2 before DI clears it, then mask interrupts: the
	; restore below puts the caller's interrupt state back exactly.
	ld	a,i
	di
	ld	a,#0
	jp	po,002$
	inc	a
002$:
	ld	(kos_driver_boot_saved_iff),a

	in	a,(KOS_MEMCTL_PORT)
	ld	(kos_driver_boot_saved_latch),a

	; Enter mode 10 over bank 6.  Everything below is fetched from
	; common memory; bank 6 answers 0000h-DFFFh.
	ld	a,#(KOS_MODE_APPLICATION | KOS_DRIVER_BANK)
	out	(KOS_MEMCTL_PORT),a

	; One call into the boot entry: push the continuation, push the
	; entry, and let the entry's RET return into the continuation.  Both
	; pushes go to the caller's (common) stack, which stays visible.
	ld	bc,#kos_driver_boot_return
	push	bc
	ld	hl,(kos_driver_boot_entry)
	push	hl
	ret

kos_driver_boot_return:
	; Back to the caller's exact latch before touching anything on the
	; bank-7 side; the state below is common memory, visible either way.
	ld	a,(kos_driver_boot_saved_latch)
	out	(KOS_MEMCTL_PORT),a
	ld	a,(kos_driver_boot_saved_iff)
	or	a
	jr	z,003$
	ei
003$:
	xor	a
	ret

	.area _COMMON_DATA

; Single-depth transition state (see the module header): common memory,
; because the gate reads it back with mode 10 selected over bank 6.
kos_driver_boot_entry::
	.ds	2
kos_driver_boot_saved_latch::
	.ds	1
kos_driver_boot_saved_iff::
	.ds	1

; The loader's upcall request block (struct kos_upcall_req, 7 bytes):
; page, entry:u16, args:u16, result:u16.  Common memory, so the fixed
; upcall shim (kernel/memory/upcall.h) and the mode-11 loader both reach
; it.
_kos_driver_upcall_block::
	.ds	7
