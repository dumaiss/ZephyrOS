/* Zephyr-80 hardware constants (C visible).
 *
 * Raw I/O port numbers, latch fields, memory modes, and SIO/CTC register
 * values live here (and in hardware.inc for assembly).  Memory-region
 * geometry is declared in memory.h; IM2 vector-page and IRQ transition
 * wiring constants are declared in irq.h.  Kernel code includes
 * <zephyros/platform.h> rather than hard-coding board constants; a
 * hardware-owning driver implementation includes this narrow header
 * directly.
 */

#ifndef ZEPHYR80_HARDWARE_H
#define ZEPHYR80_HARDWARE_H

/* --- I/O ports --- */

/* Memory/banking latch (decoded across 00h-0Fh; software uses 00h). */
#define ZOS_MEMCTL_PORT             0x00u

/* Z80 SIO channel 0 */
#define ZOS_SIO0A_DATA              0x20u
#define ZOS_SIO0A_CTRL              0x21u
/* Z80 SIO channel 0, port B: early console */
#define ZOS_SIO0B_DATA              0x22u
#define ZOS_SIO0B_CTRL              0x23u
/* SIO channel 1: the IOC two-lane transport (M8).  Lane A is the bulk
 * lane; lane B is the command lane. */
#define ZOS_SIO1A_DATA              0x30u
#define ZOS_SIO1A_CTRL              0x31u
#define ZOS_SIO1B_DATA              0x32u
#define ZOS_SIO1B_CTRL              0x33u
#define ZOS_CTC0_PORT               0x40u

/* --- Memory/banking latch fields --- */

#define ZOS_BANK_MASK               0x07u   /* D2..D0 selected SRAM bank   */
#define ZOS_LATCH_NON_BANK_MASK     0xF8u   /* D7..D3 (bank update keeps)  */
#define ZOS_MEM_MODE0               0x08u   /* D3 memory-mode bit 0        */
#define ZOS_MEM_MODE1               0x10u   /* D4 memory-mode bit 1        */
#define ZOS_MODE_MASK               0x18u   /* D4:D3 memory mode           */
#define ZOS_ROM_PAGE_SHIFT          5
#define ZOS_ROM_PAGE_MASK           0xE0u   /* D7..D5 selected ROM page    */

/* Memory modes (MEM_DECODER rev 12; D4:D3 are a mapping select, not
 * feature flags).  Only mode 00 answers reads from ROM, and it answers
 * every read from ROM: code running there must be mirrored in the ROM
 * page and must not use a stack.  08h is a flat 64 KiB view of the
 * selected SRAM bank with no ROM and no common region. */
#define ZOS_MODE_BOOT               0x00u   /* ROM access / RAM destination */
#define ZOS_MODE_FLAT               0x08u   /* flat selected SRAM bank     */
#define ZOS_MODE_APPLICATION        0x10u
#define ZOS_MODE_OS                 0x18u

/* M0 boot-ROM page assignment */
#define ZOS_BOOT_OS_ROM_PAGE        1
#define ZOS_BOOT_OS_BANK            7

/* --- SIO RR0 status bits --- */

#define ZOS_SIO_RR0_RX_AVAILABLE    0x01u
#define ZOS_SIO_RR0_TX_EMPTY        0x04u
#define ZOS_SIO_RR0_DCD             0x08u
#define ZOS_SIO_RR0_CTS             0x20u
#define ZOS_SIO_RR0_TX_UNDERRUN     0x40u
/* Only TX-empty is required before writing the data port: the console is
 * not gated by the FT230X modem-handshake crossover (see WR3/WR5 below). */
#define ZOS_SIO_TX_READY            ZOS_SIO_RR0_TX_EMPTY

/* --- SIO register programming values (115200 8N1, polled) --- */

#define ZOS_SIO_WR_SELECT_WR1         0x01u
#define ZOS_SIO_WR_SELECT_WR3         0x03u
#define ZOS_SIO_WR_SELECT_WR4         0x04u
#define ZOS_SIO_WR_SELECT_WR5         0x05u

#define ZOS_SIO_WR0_CHANNEL_RESET       0x18u
#define ZOS_SIO_WR4_X16_1STOP_NOPARITY  0x44u
/* Auto Enables off: with them on the transmitter/receiver follow CTS/DCD,
 * and on this board CTS is the FT230X RTS# line — a host that leaves RTS
 * released would silently mute the console. */
#define ZOS_SIO_WR3_RX8_NO_AUTO_ENABLES 0xC1u
/* RTS asserted: drives the FT230X CTS# input low so terminals using
 * hardware flow control are always allowed to send. */
#define ZOS_SIO_WR5_TX8_DTR_RTS_ASSERT  0xEAu
#define ZOS_SIO_WR1_INT_DISABLED        0x00u

/* --- IOC command lane (SIO1/B, External Sync) ---------------------------
 *
 * Verified against the deployed Zephyr-80 host transport (level 08h) and
 * its MCU peer.  Declarative register/status values, packet constants,
 * polling budgets, scan/FIFO bounds, and link-level markers only: the
 * mechanisms live in drivers/ioc/.  The MCU is the clock master; /DCDB is
 * COMMAND_READY, /RTSB is the held "unacknowledged request" level, and
 * /CTSB is the HID input doorbell. */

/* RR0 semantic masks on the command lane. */
#define ZOS_IOC_CMD_READY_MASK      0x08u /* /DCDB: MCU command-idle      */
#define ZOS_IOC_HID_DOORBELL_MASK   0x20u /* /CTSB: HID bytes queued      */

/* Command-lane WR0 commands. */
#define ZOS_IOC_WR0_RESET_CHANNEL     0x18u
#define ZOS_IOC_WR0_RESET_EXT_STATUS  0x10u
#define ZOS_IOC_WR0_RESET_ERROR       0x30u
#define ZOS_IOC_WR0_RESET_EOM         0xC0u

/* WR7 register select (the other selects reuse the SIO group above). */
#define ZOS_IOC_WR_SELECT_WR7       0x07u

/* WR1: no interrupts, no WAIT/DMA. */
#define ZOS_IOC_WR1_INT_WAIT_OFF    0x00u

/* WR3 receive modes: 8-bit RX, auto enables off, RX CRC disabled. */
#define ZOS_IOC_WR3_RX_OFF          0xC0u /* receiver disabled            */
#define ZOS_IOC_WR3_RX_HUNT         0xD1u /* enable + Enter Hunt Phase    */
#define ZOS_IOC_WR3_RX_NO_HUNT      0xC1u /* enable, boundary preserved   */

/* WR4: External Sync mode, x1 clock, no parity. */
#define ZOS_IOC_WR4_EXTSYNC_X1      0x30u

/* WR5: 8-bit TX, DTR set, TX CRC disabled, RTS managed separately.
 * Releasing RTS means disabling TX (00h): an External-Sync transmitter
 * never reports empty, so clearing the RTS bit alone leaves /RTS low. */
#define ZOS_IOC_WR5_TX_RTS_OFF      0x68u
#define ZOS_IOC_WR5_TX_RTS_ON       0x6Au
#define ZOS_IOC_WR5_IDLE            0x00u

/* WR7: transmit underrun fill (marking).  Never the A5 packet marker. */
#define ZOS_IOC_WR7_UNDERRUN_FILL   0xFFu

/* Common wire packet: A5 5A LEN_LO LEN_HI TYPE SEQ STATUS DATA... CRC. */
#define ZOS_IOC_PACKET_SYNC0        0xA5u
#define ZOS_IOC_PACKET_SYNC1        0x5Au
#define ZOS_IOC_PACKET_FIXED_LEN    0x03u /* TYPE + SEQ + STATUS          */
#define ZOS_IOC_PACKET_CRC_BYTES    0x02u
#define ZOS_IOC_CMD_MAX_DATA        0x1Au /* 26-byte command DATA maximum */

/* Command-request trailer: two FFh bytes clocked before RTS release so
 * the last frame byte has left the transmitter when TX is disabled. */
#define ZOS_IOC_SEND_TRAILER_BYTES  0x02u
#define ZOS_IOC_SEND_TRAILER_FILL   0xFFu

/* Bounded poll budgets (24-bit countdown = inner x outer). */
#define ZOS_IOC_CMD_READY_TIMEOUT   0xFFFFu
#define ZOS_IOC_CMD_READY_OUTER     0x08u
#define ZOS_IOC_CMD_BYTE_TIMEOUT    0xFFFFu
#define ZOS_IOC_CMD_BYTE_OUTER      0x08u

/* Reply marker scan window and SIO receive FIFO bound. */
#define ZOS_IOC_CMD_REPLY_SCAN_LIMIT 0xA0u
#define ZOS_IOC_RX_FIFO_DEPTH       0x03u
#define ZOS_IOC_RX_DRAIN_LIMIT      0x04u /* FIFO depth + 1               */

/* Link establishment: CMD_LINK_SYNC re-arms both lanes; the reply
 * carries the falling /SYNC edge the command receiver needs once. */
#define ZOS_IOC_CMD_LINK_SYNC       0x0Cu
#define ZOS_IOC_RSP_LINK_SYNC       0x8Cu
#define ZOS_IOC_LINK_BRINGUP_TRIES  0x04u

/* --- IOC bulk lane (SIO1/A, External Sync) -----------------------------
 *
 * Ports 30h/31h.  Same common packet envelope and persistent External
 * Sync as the command lane; DATA is 1-512 bytes.  The deployed pair runs
 * with bulk DATA CRC bypassed on both sides: the MCU sends a zeroed
 * trailer and ignores the host's trailer.  Framing, marker, header and
 * admission validation are never bypassed, and the host still consumes
 * the two trailer bytes. */

/* WR0 commands used by the bulk lane in addition to the shared ones. */
#define ZOS_IOC_WR0_RESET_RX_CRC    0x40u /* receive CRC generator reset  */
#define ZOS_IOC_WR0_RESET_TX_CRC    0x80u /* transmit CRC generator reset */

/* WR6 register select (WR7 reuses ZOS_IOC_WR_SELECT_WR7). */
#define ZOS_IOC_WR_SELECT_WR6       0x06u

/* WR3 bulk receive modes: 8-bit RX, auto enables off, Rx CRC enabled.
 * Hunt is used only until the first verified bulk packet; NO_HUNT keeps
 * the established character boundary.  BULK_TX is the write-direction
 * value: the receiver stays enabled while the host transmits. */
#define ZOS_IOC_WR3_BULK_RX_HUNT    0xD9u
#define ZOS_IOC_WR3_BULK_RX_NO_HUNT 0xC9u
#define ZOS_IOC_WR3_BULK_TX         0xC1u

/* WR5 bulk values: 8-bit TX, DTR, Tx CRC armed; RTS on/off/idle. */
#define ZOS_IOC_WR5_BULK_RTS_ON     0xEBu
#define ZOS_IOC_WR5_BULK_RTS_OFF    0xE9u
#define ZOS_IOC_WR5_BULK_IDLE       0xE0u /* TX disabled, RTS off         */

/* WR6/WR7 synchronous fill.  AAh cannot contain the A5 5A marker (strict
 * bit alternation) and is not the zero a reset CRC register reads. */
#define ZOS_IOC_BULK_SYNC_FILL      0xAAu

/* Sacrificial lead-in shifted before the real marker (00h on purpose). */
#define ZOS_IOC_BULK_LEADIN         0x00u
/* Bounded marker scan for the complete A5 5A after any pipeline bytes. */
#define ZOS_IOC_BULK_SCAN_LIMIT     0x08u
/* Longest bulk DATA payload: one SD sector / one /SHARED chunk. */
#define ZOS_IOC_BULK_MAX_LEN        512u
/* Measured CRC-completion guard after the terminating underrun: two CRC
 * characters at the wire rate, with margin. */
#define ZOS_IOC_BULK_UNDERRUN_GUARD 128u

/* Deployed pair mode: bulk DATA CRC is bypassed on both ends.  The host
 * consumes the two-byte trailer and never compares it; the write path
 * still arms the SIO TX CRC generator.  The constant is declared in the
 * paired C/assembly headers so a one-sided change cannot build. */
#define ZOS_IOC_BULK_CRC_BYPASS     1u

/* --- LunchCrema V9958 video (ports A0h-A4h) ----------------------------
 *
 * Direct-hardware GRAPHIC 6 settings verified on the physical Zephyr-80
 * (LunchCrema bring-up record, MANDELV5 path): 10 MHz Z80, four 64Kx4
 * DRAMs providing 128 KiB of VDP VRAM, R#25 WTE/VDS discipline and the
 * U11 front porch.  Declarative port, latch, register and delay values
 * only: every instruction sequence, loop and behavior lives in
 * drivers/v9958/.
 *
 * The configuration latch captures D0 (VDP interrupt route) and D1
 * (/WS_EN) together, so every write uses a software shadow and preserves
 * D0.  D1=1 bypasses only the porch while the paced bootstrap runs;
 * D1=0 enables it after native WAIT is turned on. */

#define ZOS_V9958_DATA_PORT         0xA0u
#define ZOS_V9958_CMD_PORT          0xA1u
#define ZOS_V9958_PALETTE_PORT      0xA2u
#define ZOS_V9958_INDIRECT_PORT     0xA3u
#define ZOS_V9958_CONFIG_PORT       0xA4u

#define ZOS_V9958_CONFIG_INT_ROUTE  0x01u /* D0: interrupt route (preserved) */
#define ZOS_V9958_CONFIG_PORCH_OFF  0x02u /* D1=1: /WS_EN bypasses the porch */

/* Command-port register select: value first, then register | 80h. */
#define ZOS_V9958_REG_SELECT        0x80u

/* Status register 2 (selected through R#15) bit 0: command engine busy. */
#define ZOS_V9958_STATUS2_CE        0x01u

/* Command-engine register block R#32..R#46, streamed through A3h. */
#define ZOS_V9958_CMD_REGISTER_BASE  32u
#define ZOS_V9958_CMD_REGISTER_COUNT 15u

/* VDP register numbers used by the proven GRAPHIC 6 bring-up. */
#define ZOS_V9958_REG_R0            0u
#define ZOS_V9958_REG_R1            1u
#define ZOS_V9958_REG_R2            2u
#define ZOS_V9958_REG_R3            3u
#define ZOS_V9958_REG_R4            4u
#define ZOS_V9958_REG_R5            5u
#define ZOS_V9958_REG_R6            6u
#define ZOS_V9958_REG_R7            7u
#define ZOS_V9958_REG_R8            8u
#define ZOS_V9958_REG_R9            9u
#define ZOS_V9958_REG_R10           10u
#define ZOS_V9958_REG_R11           11u
#define ZOS_V9958_REG_R14           14u
#define ZOS_V9958_REG_R15           15u
#define ZOS_V9958_REG_R16           16u
#define ZOS_V9958_REG_R17           17u
#define ZOS_V9958_REG_R18           18u
#define ZOS_V9958_REG_R23           23u
#define ZOS_V9958_REG_R25           25u
#define ZOS_V9958_REG_R26           26u
#define ZOS_V9958_REG_R27           27u
#define ZOS_V9958_REG_R46           46u

/* Required register values from the bring-up record: R#0=0Ah (M5+M3:
 * GRAPHIC 6), R#2=1Fh (real-V9958 page-zero addressing), R#8=08h (VR=1,
 * 64Kx4 DRAMs), R#9=88h (212 lines, interlace), R#25 WTE=1/VDS=0. */
#define ZOS_V9958_R0_G6             0x0Au
#define ZOS_V9958_R1_DISPLAY_OFF    0x00u
#define ZOS_V9958_R1_DISPLAY_ON     0x40u
#define ZOS_V9958_R2_G6             0x1Fu
#define ZOS_V9958_R5_SPRITE_BASE    0xE4u
#define ZOS_V9958_R6_SPRITE_PATTERN 0x3Fu
#define ZOS_V9958_R7_BORDER         0x04u
#define ZOS_V9958_R8_64K_DRAM       0x08u
#define ZOS_V9958_R9_212_INTERLACE  0x88u
#define ZOS_V9958_R11_SPRITE_ATTR   0x03u
#define ZOS_V9958_R15_STATUS2       0x02u
#define ZOS_V9958_R23_TEXT_BASE     0xFCu /* 100h - 4-line display offset */
#define ZOS_V9958_R25_WAIT_OFF      0x00u /* WTE=0, VDS=0 (CPUCLK kept)    */
#define ZOS_V9958_R25_WAIT_ON       0x04u /* WTE=1, VDS=0 (native WAIT)    */

/* Bounded bootstrap delay count.  Every register write made while
 * R#25 WTE is disabled is spaced by this many djnz iterations; the
 * proven pacing never relies on incidental instruction timing. */
#define ZOS_V9958_BOOT_DELAY_COUNT  16u

/* Hardware-smoke host sync: diagnostic images built with
 * ZOS_SMOKE_HOST_SYNC=1 wait for this two-byte sequence on SIO0/B before
 * printing the banner, because the bench's reset line also resets the USB
 * bridge (the capture can only start once the host has reopened the
 * port).  tools/serial_smoke.py sends the sequence; the kernel scan
 * discards everything that does not match. */
#define ZOS_SMOKE_HOST_SYNC_FIRST   0xAAu
#define ZOS_SMOKE_HOST_SYNC_SECOND  0x55u

/* Finite TX-ready poll budget for the early console.  A missing or
 * disconnected terminal must not hang the kernel forever. */
#define ZOS_SIO_TX_TIMEOUT          0xFFFFu

/* M2 CTC0 interrupt bring-up (physical channel 0 only).  The CTC0 vector
 * mapping is declared in irq.h; the bank-7 ISR stack bounds in memory.h. */
#define ZOS_CTC0_CONTROL_TEST       0xA7u
#define ZOS_CTC0_TIME_CONSTANT      0x00u
#define ZOS_CTC0_RESET              0x03u

#endif /* ZEPHYR80_HARDWARE_H */
