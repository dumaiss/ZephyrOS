; -----------------------------------------------------------------------------
; Zephyr-80 Mk2 memory-map constants (assembly visible).
;
; The common page (architecture 2.7.2): DAT entries 7 and 15 name the same
; SRAM page, so logical E000h-FFFFh is the same memory in the system and
; the working bank.  It keeps the Mk1 common layout (architecture 2.4) and
; adds the DAT state: the software mirrors of both banks and the control
; register, and the identity of the image the working bank holds.  IM2
; vector-page and IRQ transition constants live in irq.inc.
;
; Keep this file value-identical with memory.h (parity-tested).
; -----------------------------------------------------------------------------

; The low entry stubs every page-0 image carries (0000h-00FFh).
KOS_PAGE_ZERO_END       = 0x0100
KOS_NMI_ENTRY           = 0x0066

; The system DAT after the boot (architecture 2.7.6): entry 0 the system
; page-0 image (a caller's page 0 for a syscall crossing), entries 1-4 the
; nucleus pages, never remapped, entries 5-6 the copy windows below,
; entry 7 the common page.  The nucleus links inside KOS_NUCLEUS_BASE-
; KOS_NUCLEUS_END; SRAM KOS_NUCLEUS_FIRST_PAGE + n backs its logical page
; KOS_NUCLEUS_FIRST_ENTRY + n.
KOS_NUCLEUS_BASE        = 0x2000
KOS_NUCLEUS_END         = 0xA000
KOS_NUCLEUS_FIRST_ENTRY = 1
KOS_NUCLEUS_ENTRIES     = 4
KOS_SYSTEM_PAGE0        = 0x40
KOS_NUCLEUS_FIRST_PAGE  = 0x41
KOS_COMMON_PAGE         = 0x7F

; The ROM image: 512 KiB, 64 pages.  ROM page L (00h-07h) holds the
; linked image's logical page L: 00h the reset and boot view (the
; bootstrap and the low entry stubs), 01h-04h the nucleus, 07h the common
; page.  The recovery store follows, read through the copy windows.
KOS_ROM_IMAGE_SIZE      = 0x80000
KOS_ROM_IMAGE_PAGES     = 0x08
KOS_ROMFS_FIRST_PAGE    = 0x08
KOS_ROMFS_PAGE_COUNT    = 36
KOS_ROMFS_WINDOW_SIZE   = 0x2000
KOS_ROMFS_CAPACITY      = 0x48000
KOS_ROMFS_STAGING_SIZE  = 128

; The system bank's ISR stack: the top of logical page 2, a nucleus page.
KOS_IRQ_ISR_STACK_BASE  = 0x5F00
KOS_IRQ_ISR_STACK_TOP   = 0x6000
KOS_IRQ_ISR_STACK_SIZE  = 0x0100

; The common page.  The packed _COMMON + _COMMON_DATA run starts at
; KOS_COMMON_BASE and may not exceed KOS_COMMON_CEILING bytes.
KOS_COMMON_BASE         = 0xE000
KOS_COMMON_CEILING      = 0x0C00

; Common boot/runtime stack: FE88h-FFFFh, grown down from FFFFh (initial
; SP 0000h), above the IRQ transition reservation (irq.inc).
KOS_STACK_INIT_SP       = 0x0000

; The syscall gate stack in the common data area (the Mk1 size).
KOS_GATE_STACK_SIZE     = 384

; The real-time interrupt: 0066h of every page-0 image jumps to the
; vector, a JP to the owner's handler or to the idle RETN after it.
KOS_RT_VECTOR           = 0xEF08
KOS_RT_IDLE             = 0xEF0B

; DAT state.  The mirrors are written before every mapper OUT, so they
; always describe the mapping in force or the one the next OUT installs.
;   KOS_DAT_SYSTEM    entries 0-7, one page byte each
;   KOS_DAT_WORKING   entries 8-15
;   KOS_DAT_CONTROL   the control register
;   KOS_DAT_CACHED    the 8-byte image the working bank holds, or a high
;                     byte of 0 once an entry differs from every image
KOS_DAT_STATE_BASE      = 0xEF10
KOS_DAT_SYSTEM          = 0xEF10
KOS_DAT_WORKING         = 0xEF18
KOS_DAT_CONTROL         = 0xEF20
KOS_DAT_CACHED          = 0xEF21
KOS_DAT_STATE_SIZE      = 0x0013

; The nucleus's copy windows: two system entries it remaps to move bytes
; between pages (an object, a caller's buffer, a program image) without
; staging them.  No interrupt path uses them.
KOS_DAT_COPY_SRC_ENTRY  = 5
KOS_DAT_COPY_SRC_BASE   = 0xA000
KOS_DAT_COPY_DST_ENTRY  = 6
KOS_DAT_COPY_DST_BASE   = 0xC000

; The far window (logical 8000h-BFFFh): working entries 12-13.
KOS_DAT_FAR_ENTRY       = 12
KOS_DAT_FAR_BASE        = 0x8000
