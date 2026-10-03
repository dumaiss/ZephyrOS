; -----------------------------------------------------------------------------
; ZephyrOS M0/M1 memory-map constants (assembly visible).
;
; These values are architectural invariants; see docs/architecture.md and
; docs/boot.md.  The linker script and tools/pack_image.py enforce them.
; -----------------------------------------------------------------------------

; Page-zero/reset reservation in ROM page 0.  The stackless reset copier
; must stay below ZOS_RESET_TOP so that M1 can install RST 30h there.
ZOS_RESET_TOP           = 0x0030
ZOS_PAGE_ZERO_END       = 0x0100

; Bank-7 OS body, visible in mode 11.  The loadable (ROM-initialized) part
; must end below ZOS_BANK7_LOAD_END; C000h-DFFFh is zero-initialized runtime
; area that is never loaded from ROM.
ZOS_BANK7_BASE          = 0x2000
ZOS_BANK7_LOAD_END      = 0xC000
ZOS_BANK7_RUNTIME_END   = 0xDF00
ZOS_IRQ_ISR_STACK_BASE  = 0xDF00
ZOS_IRQ_ISR_STACK_TOP   = 0xE000
ZOS_IRQ_ISR_STACK_SIZE  = 0x0100

; Permanent common code/data in physical SRAM bank 0.
ZOS_COMMON_BASE         = 0xE000
ZOS_COMMON_CODE_END     = 0xFD00
ZOS_IRQ_VECTOR_BASE     = 0xFD00
ZOS_IRQ_VECTOR_SIZE     = 0x0101
ZOS_IRQ_STATE_BASE      = 0xFE01
ZOS_IRQ_STATE_HEADER_SIZE = 0x0006
ZOS_IRQ_STACK_BASE      = 0xFE80
ZOS_IRQ_STACK_TOP       = 0xFF00
ZOS_IRQ_STACK_SIZE      = 0x0080
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
