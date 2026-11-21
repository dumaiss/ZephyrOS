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
