; -----------------------------------------------------------------------------
; Zephyr-80 hardware constants (assembly visible).
;
; Raw I/O port numbers, latch fields, memory modes, and SIO/CTC register
; values.  Memory-region geometry is declared in memory.inc; IM2 vector-page
; and IRQ transition wiring constants are declared in irq.inc.
;
; Keep this file value-identical with hardware.h (parity-tested).
; -----------------------------------------------------------------------------

; --- I/O ports ---

ZOS_MEMCTL_PORT         = 0x00

ZOS_SIO0A_DATA          = 0x20
ZOS_SIO0A_CTRL          = 0x21
ZOS_SIO0B_DATA          = 0x22
ZOS_SIO0B_CTRL          = 0x23
ZOS_SIO1A_DATA          = 0x30
ZOS_SIO1A_CTRL          = 0x31
ZOS_SIO1B_DATA          = 0x32
ZOS_SIO1B_CTRL          = 0x33
ZOS_CTC0_PORT           = 0x40

; --- Memory/banking latch fields ---

ZOS_BANK_MASK           = 0x07
ZOS_LATCH_NON_BANK_MASK = 0xF8
ZOS_MEM_MODE0           = 0x08
ZOS_MEM_MODE1           = 0x10
ZOS_MODE_MASK           = 0x18
ZOS_ROM_PAGE_SHIFT      = 5
ZOS_ROM_PAGE_MASK       = 0xE0

; Memory modes (MEM_DECODER rev 12; D4:D3 are a mapping select, not
; feature flags).  Only mode 00 answers reads from ROM, and it answers
; every read from ROM: code running there must be mirrored in the ROM
; page and must not use a stack.  08h is a flat 64 KiB view of the
; selected SRAM bank with no ROM and no common region.
ZOS_MODE_BOOT           = 0x00
ZOS_MODE_FLAT           = 0x08
ZOS_MODE_APPLICATION    = 0x10
ZOS_MODE_OS             = 0x18

; M0 boot-ROM page assignment
ZOS_BOOT_OS_ROM_PAGE    = 1
ZOS_BOOT_OS_BANK        = 7

; --- SIO RR0 status bits ---

ZOS_SIO_RR0_RX_AVAILABLE = 0x01
ZOS_SIO_RR0_TX_EMPTY     = 0x04
ZOS_SIO_RR0_DCD          = 0x08
ZOS_SIO_RR0_CTS          = 0x20
ZOS_SIO_RR0_TX_UNDERRUN  = 0x40
ZOS_SIO_TX_READY         = ZOS_SIO_RR0_TX_EMPTY

; --- SIO register programming values (115200 8N1, polled) ---

ZOS_SIO_WR_SELECT_WR1    = 0x01
ZOS_SIO_WR_SELECT_WR3    = 0x03
ZOS_SIO_WR_SELECT_WR4    = 0x04
ZOS_SIO_WR_SELECT_WR5    = 0x05

ZOS_SIO_WR0_CHANNEL_RESET       = 0x18
ZOS_SIO_WR4_X16_1STOP_NOPARITY  = 0x44
ZOS_SIO_WR3_RX8_NO_AUTO_ENABLES = 0xC1
ZOS_SIO_WR5_TX8_DTR_RTS_ASSERT  = 0xEA
ZOS_SIO_WR1_INT_DISABLED        = 0x00

; --- IOC command lane (SIO1/B, External Sync) ---------------------------
;
; Verified against the deployed Zephyr-80 host transport (level 08h) and
; its MCU peer.  Declarative register/status values, packet constants,
; polling budgets, scan/FIFO bounds, and link-level markers only: the
; mechanisms live in drivers/ioc/.  The MCU is the clock master; /DCDB is
; COMMAND_READY, /RTSB is the held "unacknowledged request" level, and
; /CTSB is the HID input doorbell.

ZOS_IOC_CMD_READY_MASK      = 0x08
ZOS_IOC_HID_DOORBELL_MASK   = 0x20

ZOS_IOC_WR0_RESET_CHANNEL     = 0x18
ZOS_IOC_WR0_RESET_EXT_STATUS  = 0x10
ZOS_IOC_WR0_RESET_ERROR       = 0x30
ZOS_IOC_WR0_RESET_EOM         = 0xC0

ZOS_IOC_WR_SELECT_WR7       = 0x07

ZOS_IOC_WR1_INT_WAIT_OFF    = 0x00

ZOS_IOC_WR3_RX_OFF          = 0xC0
ZOS_IOC_WR3_RX_HUNT         = 0xD1
ZOS_IOC_WR3_RX_NO_HUNT      = 0xC1

ZOS_IOC_WR4_EXTSYNC_X1      = 0x30

ZOS_IOC_WR5_TX_RTS_OFF      = 0x68
ZOS_IOC_WR5_TX_RTS_ON       = 0x6A
ZOS_IOC_WR5_IDLE            = 0x00

ZOS_IOC_WR7_UNDERRUN_FILL   = 0xFF

ZOS_IOC_PACKET_SYNC0        = 0xA5
ZOS_IOC_PACKET_SYNC1        = 0x5A
ZOS_IOC_PACKET_FIXED_LEN    = 0x03
ZOS_IOC_PACKET_CRC_BYTES    = 0x02
ZOS_IOC_CMD_MAX_DATA        = 0x1A

ZOS_IOC_SEND_TRAILER_BYTES  = 0x02
ZOS_IOC_SEND_TRAILER_FILL   = 0xFF

ZOS_IOC_CMD_READY_TIMEOUT   = 0xFFFF
ZOS_IOC_CMD_READY_OUTER     = 0x08
ZOS_IOC_CMD_BYTE_TIMEOUT    = 0xFFFF
ZOS_IOC_CMD_BYTE_OUTER      = 0x08

ZOS_IOC_CMD_REPLY_SCAN_LIMIT = 0xA0
ZOS_IOC_RX_FIFO_DEPTH       = 0x03
ZOS_IOC_RX_DRAIN_LIMIT      = 0x04

ZOS_IOC_CMD_LINK_SYNC       = 0x0C
ZOS_IOC_RSP_LINK_SYNC       = 0x8C
ZOS_IOC_LINK_BRINGUP_TRIES  = 0x04

ZOS_SIO_TX_TIMEOUT      = 0xFFFF

; --- M2 CTC0 interrupt bring-up (vector mapping lives in irq.inc) ---
ZOS_CTC0_CONTROL_TEST   = 0xA7
ZOS_CTC0_TIME_CONSTANT  = 0x00
ZOS_CTC0_RESET          = 0x03
