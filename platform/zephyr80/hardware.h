/* Zephyr-80 hardware constants (C visible).
 *
 * Raw I/O port numbers, latch fields, memory modes, and SIO/CTC register
 * values live here (and in hardware.inc for assembly).  Memory-region
 * geometry is declared in memory.h; IM2 vector-page and IRQ transition
 * wiring constants are declared in irq.h.  Kernel code must not hard-code
 * port numbers or board constants; include <zephyros/platform.h> instead.
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

/* Finite TX-ready poll budget for the early console.  A missing or
 * disconnected terminal must not hang the kernel forever. */
#define ZOS_SIO_TX_TIMEOUT          0xFFFFu

/* M2 CTC0 interrupt bring-up (physical channel 0 only).  The CTC0 vector
 * mapping is declared in irq.h; the bank-7 ISR stack bounds in memory.h. */
#define ZOS_CTC0_CONTROL_TEST       0xA7u
#define ZOS_CTC0_TIME_CONSTANT      0x00u
#define ZOS_CTC0_RESET              0x03u

#endif /* ZEPHYR80_HARDWARE_H */
