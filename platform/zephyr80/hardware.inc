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

; --- IOC bulk lane (SIO1/A, External Sync) -----------------------------
;
; Ports 30h/31h.  Same common packet envelope and persistent External
; Sync as the command lane; DATA is 1-512 bytes.  The deployed pair runs
; with bulk DATA CRC bypassed on both sides; framing, marker, header and
; admission validation are never bypassed.  Values are the C pair of
; platform/zephyr80/hardware.h; the parity tests compare every shared
; name.

ZOS_IOC_WR0_RESET_RX_CRC    = 0x40
ZOS_IOC_WR0_RESET_TX_CRC    = 0x80

ZOS_IOC_WR_SELECT_WR6       = 0x06

ZOS_IOC_WR3_BULK_RX_HUNT    = 0xD9
ZOS_IOC_WR3_BULK_RX_NO_HUNT = 0xC9
ZOS_IOC_WR3_BULK_TX         = 0xC1

ZOS_IOC_WR5_BULK_RTS_ON     = 0xEB
ZOS_IOC_WR5_BULK_RTS_OFF    = 0xE9
ZOS_IOC_WR5_BULK_IDLE       = 0xE0

ZOS_IOC_BULK_SYNC_FILL      = 0xAA

ZOS_IOC_BULK_LEADIN         = 0x00
ZOS_IOC_BULK_SCAN_LIMIT     = 0x08
ZOS_IOC_BULK_MAX_LEN        = 512
ZOS_IOC_BULK_UNDERRUN_GUARD = 128
ZOS_IOC_BULK_CRC_BYPASS     = 1

; --- LunchCrema V9958 video (ports A0h-A4h) ----------------------------
;
; Direct-hardware GRAPHIC 6 settings verified on the physical Zephyr-80.
; Declarative port, latch, register and delay values only: every
; instruction sequence, loop and behavior lives in drivers/v9958/.
; Value-identical with the hardware.h pair (parity-tested).

ZOS_V9958_DATA_PORT         = 0xA0
ZOS_V9958_CMD_PORT          = 0xA1
ZOS_V9958_PALETTE_PORT      = 0xA2
ZOS_V9958_INDIRECT_PORT     = 0xA3
ZOS_V9958_CONFIG_PORT       = 0xA4

ZOS_V9958_CONFIG_INT_ROUTE  = 0x01
ZOS_V9958_CONFIG_PORCH_OFF  = 0x02

ZOS_V9958_REG_SELECT        = 0x80
ZOS_V9958_STATUS2_CE        = 0x01

ZOS_V9958_CMD_REGISTER_BASE  = 32
ZOS_V9958_CMD_REGISTER_COUNT = 15

ZOS_V9958_REG_R0            = 0
ZOS_V9958_REG_R1            = 1
ZOS_V9958_REG_R2            = 2
ZOS_V9958_REG_R3            = 3
ZOS_V9958_REG_R4            = 4
ZOS_V9958_REG_R5            = 5
ZOS_V9958_REG_R6            = 6
ZOS_V9958_REG_R7            = 7
ZOS_V9958_REG_R8            = 8
ZOS_V9958_REG_R9            = 9
ZOS_V9958_REG_R10           = 10
ZOS_V9958_REG_R11           = 11
ZOS_V9958_REG_R14           = 14
ZOS_V9958_REG_R15           = 15
ZOS_V9958_REG_R16           = 16
ZOS_V9958_REG_R17           = 17
ZOS_V9958_REG_R18           = 18
ZOS_V9958_REG_R23           = 23
ZOS_V9958_REG_R25           = 25
ZOS_V9958_REG_R26           = 26
ZOS_V9958_REG_R27           = 27
ZOS_V9958_REG_R46           = 46

ZOS_V9958_R0_G6             = 0x0A
ZOS_V9958_R1_DISPLAY_OFF    = 0x00
ZOS_V9958_R1_DISPLAY_ON     = 0x40
ZOS_V9958_R2_G6             = 0x1F
ZOS_V9958_R5_SPRITE_BASE    = 0xE4
ZOS_V9958_R6_SPRITE_PATTERN = 0x3F
ZOS_V9958_R7_BORDER         = 0x04
ZOS_V9958_R8_64K_DRAM       = 0x08
ZOS_V9958_R9_212_INTERLACE  = 0x88
ZOS_V9958_R11_SPRITE_ATTR   = 0x03
ZOS_V9958_R15_STATUS2       = 0x02
ZOS_V9958_R23_TEXT_BASE     = 0xFC
ZOS_V9958_R25_WAIT_OFF      = 0x00
ZOS_V9958_R25_WAIT_ON       = 0x04

ZOS_V9958_BOOT_DELAY_COUNT  = 16

; Hardware-smoke host sync sequence (kernel/main.c kernel_host_sync_wait).
ZOS_SMOKE_HOST_SYNC_FIRST   = 0xAA
ZOS_SMOKE_HOST_SYNC_SECOND  = 0x55

ZOS_SIO_TX_TIMEOUT      = 0xFFFF

; --- M2 CTC0 interrupt bring-up (vector mapping lives in irq.inc) ---
ZOS_CTC0_CONTROL_TEST   = 0xA7
ZOS_CTC0_TIME_CONSTANT  = 0x00
ZOS_CTC0_RESET          = 0x03
