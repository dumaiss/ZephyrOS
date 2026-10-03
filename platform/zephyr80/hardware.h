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
#define ZOS_CTC0_PORT               0x40u

/* --- Memory/banking latch fields --- */

#define ZOS_BANK_MASK               0x07u   /* D2..D0 selected SRAM bank   */
#define ZOS_LATCH_NON_BANK_MASK     0xF8u   /* D7..D3 (bank update keeps)  */
#define ZOS_RAM_SHADOW              0x08u   /* D3 RAM shadow / OS mode     */
#define ZOS_ROM_DIS                 0x10u   /* D4 RAM-only runtime         */
#define ZOS_MODE_MASK               0x18u   /* D4:D3 memory mode           */
#define ZOS_ROM_PAGE_SHIFT          5
#define ZOS_ROM_PAGE_MASK           0xE0u   /* D7..D5 selected ROM page    */

/* Memory modes */
#define ZOS_MODE_BOOT               0x00u
#define ZOS_MODE_SHADOW             0x08u
#define ZOS_MODE_APPLICATION        0x10u
#define ZOS_MODE_OS                 0x18u

/* M0 boot-ROM page assignment */
#define ZOS_BOOT_OS_ROM_PAGE        1
#define ZOS_BOOT_OS_BANK            7

/* --- SIO RR0 status bits --- */

#define ZOS_SIO_RR0_RX_AVAILABLE    0x01u
#define ZOS_SIO_RR0_TX_EMPTY        0x04u
#define ZOS_SIO_RR0_CTS             0x20u
/* Both TX-empty and CTS are required before writing the data port. */
#define ZOS_SIO_TX_READY            (ZOS_SIO_RR0_TX_EMPTY | ZOS_SIO_RR0_CTS)

/* --- SIO register programming values (115200 8N1, polled) --- */

#define ZOS_SIO_WR_SELECT_WR1         0x01u
#define ZOS_SIO_WR_SELECT_WR3         0x03u
#define ZOS_SIO_WR_SELECT_WR4         0x04u
#define ZOS_SIO_WR_SELECT_WR5         0x05u

#define ZOS_SIO_WR0_CHANNEL_RESET       0x18u
#define ZOS_SIO_WR4_X16_1STOP_NOPARITY  0x44u
#define ZOS_SIO_WR3_RX8_AUTO_ENABLES    0xE1u
#define ZOS_SIO_WR5_TX8_DTR_RTS_RELEASE 0xE8u
#define ZOS_SIO_WR1_INT_DISABLED        0x00u

/* Finite TX-ready poll budget for the early console.  A missing or
 * disconnected terminal must not hang the kernel forever. */
#define ZOS_SIO_TX_TIMEOUT          0xFFFFu

/* M2 CTC0 interrupt bring-up (physical channel 0 only).  The CTC0 vector
 * mapping is declared in irq.h; the bank-7 ISR stack bounds in memory.h. */
#define ZOS_CTC0_CONTROL_TEST       0xA7u
#define ZOS_CTC0_TIME_CONSTANT      0x00u
#define ZOS_CTC0_RESET              0x03u

#endif /* ZEPHYR80_HARDWARE_H */
