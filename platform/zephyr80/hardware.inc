; -----------------------------------------------------------------------------
; Zephyr-80 hardware constants (assembly visible).
;
; Keep this file value-identical with hardware.h.
; -----------------------------------------------------------------------------

; --- I/O ports ---

ZOS_MEMCTL_PORT         = 0x00

ZOS_SIO0A_DATA          = 0x20
ZOS_SIO0A_CTRL          = 0x21
ZOS_SIO0B_DATA          = 0x22
ZOS_SIO0B_CTRL          = 0x23
ZOS_CTC0_PORT           = 0x40

; --- Memory/banking latch fields ---

ZOS_BANK_MASK           = 0x07
ZOS_LATCH_NON_BANK_MASK = 0xF8
ZOS_RAM_SHADOW          = 0x08
ZOS_ROM_DIS             = 0x10
ZOS_MODE_MASK           = 0x18
ZOS_ROM_PAGE_SHIFT      = 5
ZOS_ROM_PAGE_MASK       = 0xE0

; Memory modes
ZOS_MODE_BOOT           = 0x00
ZOS_MODE_SHADOW         = 0x08
ZOS_MODE_APPLICATION    = 0x10
ZOS_MODE_OS             = 0x18

; M0 boot-ROM page assignment
ZOS_BOOT_OS_ROM_PAGE    = 1
ZOS_BOOT_OS_BANK        = 7

; --- SIO RR0 status bits ---

ZOS_SIO_RR0_RX_AVAILABLE = 0x01
ZOS_SIO_RR0_TX_EMPTY     = 0x04
ZOS_SIO_RR0_CTS          = 0x20
ZOS_SIO_TX_READY         = ZOS_SIO_RR0_TX_EMPTY | ZOS_SIO_RR0_CTS

; --- SIO register programming values (115200 8N1, polled) ---

ZOS_SIO_WR_SELECT_WR1    = 0x01
ZOS_SIO_WR_SELECT_WR3    = 0x03
ZOS_SIO_WR_SELECT_WR4    = 0x04
ZOS_SIO_WR_SELECT_WR5    = 0x05

ZOS_SIO_WR0_CHANNEL_RESET       = 0x18
ZOS_SIO_WR4_X16_1STOP_NOPARITY  = 0x44
ZOS_SIO_WR3_RX8_AUTO_ENABLES    = 0xE1
ZOS_SIO_WR5_TX8_DTR_RTS_RELEASE = 0xE8
ZOS_SIO_WR1_INT_DISABLED        = 0x00

ZOS_SIO_TX_TIMEOUT      = 0xFFFF

; --- M2 CTC0 interrupt bring-up ---
ZOS_CTC0_VECTOR_BASE    = 0x00
ZOS_CTC0_CONTROL_TEST   = 0xA7
ZOS_CTC0_TIME_CONSTANT  = 0x00
ZOS_CTC0_RESET          = 0x03
ZOS_IM2_I_VALUE         = 0xFD

; --- M2 bank-7 ISR stack (mirrors memory.inc) ---
ZOS_IRQ_ISR_STACK_BASE  = 0xDF00
ZOS_IRQ_ISR_STACK_TOP   = 0xE000
