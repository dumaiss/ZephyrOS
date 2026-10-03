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

; Bank-7 OS body, visible in mode 11.  The loadable (ROM-initialized) part
; must end below ZOS_DRIVER_ARENA_BASE; C000h-DFFFh is zero-initialized
; runtime area that is never loaded from ROM.
ZOS_BANK7_BASE          = 0x2000
ZOS_BANK7_LOAD_END      = 0xC000
ZOS_BANK7_RUNTIME_END   = 0xDF00
ZOS_IRQ_ISR_STACK_BASE  = 0xDF00
ZOS_IRQ_ISR_STACK_TOP   = 0xE000
ZOS_IRQ_ISR_STACK_SIZE  = 0x0100

; M4 bank-7 driver arena: 16 contiguous 512-byte granules reserved between
; the loadable kernel image and the zero-initialized runtime area.  Kernel
; sections, stacks, and ROM payloads must never occupy it; only the module
; loader allocates granules inside it at runtime.  Loader code must use the
; linked __driver_arena_start/__driver_arena_end symbols, not these values.
ZOS_DRIVER_ARENA_BASE   = 0xA000
ZOS_DRIVER_ARENA_END    = 0xC000
ZOS_DRIVER_GRANULE_SIZE = 0x0200
ZOS_DRIVER_GRANULES     = 16

; Permanent common code/data in physical SRAM bank 0.  ZOS_COMMON_CODE_END
; is the exclusive end of ordinary common content; the IM2 vector page and
; IRQ transition state reservations (FD00h-FEFFh) are declared in irq.inc.
ZOS_COMMON_BASE         = 0xE000
ZOS_COMMON_CODE_END     = 0xFD00
ZOS_COMMON_STACK_BASE   = 0xFF00

; Common boot/runtime stack: FF00h-FFFFh, grown down from FFFFh.
; Initial SP = 0000h (top-exclusive at 10000h).
ZOS_STACK_INIT_SP       = 0x0000

; Small common gate stack used only while crossing between mode 10 and
; mode 11.  Lives in the common data area, below the boot stack.
ZOS_GATE_STACK_SIZE     = 64

; ROM geometry of the 128 KiB boot image.
ZOS_ROM_PAGE_SIZE       = 0x10000
ZOS_BOOT_IMAGE_SIZE     = 0x20000
