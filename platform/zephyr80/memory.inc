; -----------------------------------------------------------------------------
; ZephyrOS M0-M4 memory-map constants (assembly visible).
;
; Region geometry invariants; see docs/architecture.md and docs/boot.md.
; The linker script and tools/pack_image.py enforce them.  IM2 vector-page
; and IRQ transition wiring constants live in irq.inc; hardware ports,
; latch fields, and register constants live in hardware.inc.
; -----------------------------------------------------------------------------

; Page-zero/reset reservation in ROM page 0.  The stackless reset copier
; must stay below ZOS_RESET_TOP so that M1 can install RST 30h there.
ZOS_RESET_TOP           = 0x0030
ZOS_PAGE_ZERO_END       = 0x0100

; Bank-7 OS body, visible in mode 11 under decoder revision 13.  Mode 11
; splits bank 7 into a 16 KiB fixed core and a 32 KiB cold half behind
; the 6000h window (architecture 36.2, 36.5):
;
;   2000h-5FFFh  fixed core: code that executes, and data that is
;                touched, while the window shows another bank, plus the
;                ISR stack and the service stack.
;   6000h-DFFFh  cold half, visible while the window shows bank 7
;                (P = 0): syscall-side and boot code, the driver arena
;                and the objects that move to bank 6 in stages 4-5.
;
; The fixed core's loadable (ROM-initialized) part ends at the linked
; end of the fixed code areas; its zero-initialized runtime data begins
; at the derived data base
; (align_down(ZOS_SERVICE_STACK_BASE - fixed_data_size, 512)) and is
; never loaded from ROM.  In the cold half the driver arena sits between
; the granule-aligned end of the window code and the derived window data
; base (align_down(ZOS_BANK7_RUNTIME_END - window_data_size, 512)).
ZOS_BANK7_BASE          = 0x2000
ZOS_REV13_WINDOW_BASE   = 0x6000
ZOS_BANK7_RUNTIME_END   = 0xE000
ZOS_IRQ_ISR_STACK_BASE  = 0x5F00
ZOS_IRQ_ISR_STACK_TOP   = 0x6000
ZOS_IRQ_ISR_STACK_SIZE  = 0x0100

; Service stack: 512 bytes in the fixed core, reserved now and used by
; the IPC substrate from stage 3 (architecture 36.9, 36.20).  It grows
; down from ZOS_SERVICE_STACK_TOP, below the relocated ISR stack, so the
; fixed core's runtime data never reaches either stack.
ZOS_SERVICE_STACK_BASE  = 0x5D00
ZOS_SERVICE_STACK_TOP   = 0x5F00
ZOS_SERVICE_STACK_SIZE  = 0x0200

; Aperture-copy stack reservation (work package 39, correction 01).  The
; path-B copy primitive runs its LDIR with the service stack unmapped -
; mode 10 over the caller's bank does not show bank 7 - so it moves SP
; into common memory for the length of the window.  An interrupt taken in
; the window pushes its return address, and the CTC stub its AF, below
; this top; nothing else may use the reservation.
ZOS_IPC_APERTURE_STACK_SIZE = 4

; Driver arena.  There is one geometry: the two-pass build derives the
; arena start from the granule-aligned end of the linked window code and
; the arena end from the window-data size, exporting __kernel_code_end /
; __driver_arena_start / __driver_arena_end through kernel/module/arena.asm
; and the generated build/geometry/arena_geometry.inc (its memory.inc
; variant additionally carries both data bases, the granule count and the
; fixed-core ledger fields).  The probe link places no arena.  There is
; no granule floor: the arena only has to hold the declared boot set,
; which tools/zmodpack.py's plan and tools/pack_image.py enforce.  The
; window data base is never part of the allocator.
ZOS_DRIVER_GRANULE_SIZE = 0x0200

; Permanent common code/data in physical SRAM bank 0.  ZOS_COMMON_CODE_END
; is the exclusive end of ordinary common content; the IM2 vector page and
; IRQ transition state reservations (FD00h-FEFFh) are declared in irq.inc.
; ZOS_COMMON_CEILING bounds the packed _COMMON + _COMMON_DATA run, with
; ZOS_COMMON_M10_RESERVATION held back for M10's far-call gate/state, so
; M9 enforces used <= ceiling - reservation (work package 30c).
ZOS_COMMON_BASE         = 0xE000
ZOS_COMMON_CODE_END     = 0xFD00
ZOS_COMMON_STACK_BASE   = 0xFF00
ZOS_COMMON_CEILING          = 0x0C00
ZOS_COMMON_M10_RESERVATION  = 0x0200

; Common boot/runtime stack: FF00h-FFFFh, grown down from FFFFh.
; Initial SP = 0000h (top-exclusive at 10000h).
ZOS_STACK_INIT_SP       = 0x0000

; Common gate stack used only while crossing between mode 10 and
; mode 11.  Lives in the common data area, below the boot stack.  It
; carries the whole syscall handler chain, including a module class op
; that runs a full device transaction (M8 console read: HID -> IOC -> SIO
; needs about 180 bytes), so it is sized well above the M1 dispatch set.
ZOS_GATE_STACK_SIZE     = 256

; ROM geometry of the programmed 512 KiB image: eight 64-KiB pages.
; Pages 0 and 1 are the boot pages copied to SRAM by the reset copier;
; pages 2-7 carry the read-only recovery store (ROMFS).
ZOS_ROM_PAGE_SIZE       = 0x10000
ZOS_ROM_PAGE_COUNT      = 8
ZOS_BOOT_IMAGE_SIZE     = 0x20000
ZOS_ROM_IMAGE_SIZE      = 0x80000

; ROMFS recovery-store geometry (platform assignment).  Only the
; 0000h-BFFFh window of each recovery page is part of the logical
; stream; C000h-FFFFh is never addressed as file data.  While a page is
; visible the machine is in mode 00, so C000h-FFFFh reads come from the
; ROM page as well; the fixed ROM-access stub mirror lives there.
; Capacity = ZOS_ROMFS_PAGE_COUNT x window = 288 KiB.
ZOS_ROMFS_FIRST_PAGE    = 2
ZOS_ROMFS_PAGE_COUNT    = 6
ZOS_ROMFS_WINDOW_SIZE   = 0xC000
ZOS_ROMFS_CAPACITY      = 0x48000

; ROM-access transition block in permanent common memory
; (kernel/romfs/rom_access.asm).  A two-byte trampoline switches to
; mode 00 at ZOS_ROM_ACCESS_BASE-2 and falls through into the mirrored
; stub at ZOS_ROM_ACCESS_BASE: LDIR plus the latch-restoring OUT, then
; a JP back to ordinary common code.  The stub is byte-identical in
; common SRAM and in every recovery ROM page, so instruction fetch
; survives the mode change.
ZOS_ROMFS_STAGING_SIZE    = 128
ZOS_ROM_ACCESS_BASE       = 0xEF00
ZOS_ROM_ACCESS_SIZE       = 8
