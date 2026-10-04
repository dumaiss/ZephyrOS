/* ZephyrOS IOC client-visible service vocabulary (M8).
 *
 * The IOC class ABI lives in include/zephyros/ioc.h: the native
 * `struct zioc_frame`, the `struct zioc_ops` table, the semantic status
 * bits, and the transport error codes.  This header is the *vocabulary*
 * half of the contract: it names the deployed MCU command/reply classes
 * and the client-visible DATA payload offsets that a protocol-aware
 * client must share to use the transport.  It is service vocabulary,
 * not another device interface.
 *
 * The wire framing, the rolling sequence, CRC handling, lane setup, the
 * READY/DONE bulk admission state, bulk recovery, the frozen bulk-arming
 * class gate, and every pinned-driver implementation detail remain
 * private to drivers/ioc/.  Ordinary clients use `struct zioc_frame` /
 * `struct zioc_ops` and never include this header.  Protocol-aware
 * clients -- the HID driver and the M8 target diagnostics -- include it
 * to name command types and decode payload offsets as native-frame DATA
 * offsets.
 *
 * The numeric values pin the deployed Zephyr-80 IOC firmware, which is
 * never modified; this header documents what it advertises.  Payload
 * offsets are DATA offsets (compatibility-mailbox offset minus the four
 * mailbox header bytes).
 */

#ifndef ZEPHYROS_IOC_PROTOCOL_H
#define ZEPHYROS_IOC_PROTOCOL_H

/* PING command and reply.  The request carries an echo pattern; the
 * reply echoes those bytes and appends diagnostics. */
#define IOC_CMD_PING          0x01u
#define IOC_RSP_PING          0x81u

/* PING request/reply DATA layout. */
#define IOC_PING_ECHO_BYTES   16u /* echoed request bytes             */
#define IOC_PING_DATA_ECHO    0u
#define IOC_PING_DATA_LEVEL   16u /* controller firmware level        */
#define IOC_PING_DATA_POWER   17u /* power/link handshake snapshot    */
#define IOC_PING_DATA_LEN     26u /* reply declares the full payload  */

/* IOC_PING_DATA_POWER bit: the MCU reports the command lane as
 * established.  The pinned deployment drives /SYNC low for the session
 * once synchronised. */
#define IOC_PING_LINK_SYNCED  0x40u

/* Pinned deployment: controller firmware level 74 (4Ah).  Reported by
 * PING; recorded and validated by the M8 target diagnostic.  A driver
 * never *requires* this value, it reports what the controller returns. */
#define IOC_FW_LEVEL_PINNED   74u

/* HID input fetch (CMD_HID_INPUT / RSP_HID_INPUT).
 *
 * The one-byte request DATA is the maximum number of translated bytes
 * wanted.  The reply DATA begins with the MCU's `queued` counter and
 * its saturating `dropped` counter, followed by up to
 * IOC_HID_INPUT_MAX_DATA terminal bytes.  The controller DEQUEUES the
 * bytes it returns, so a fetch must ask for exactly the free space of
 * its local buffer: anything that does not fit locally is lost rather
 * than left queued.  Reply DATA length is therefore
 * IOC_HID_INPUT_META_LEN plus 0..IOC_HID_INPUT_MAX_DATA bytes.  The
 * translation is the MCU's; these bytes are terminal input, not raw
 * USB reports. */
#define IOC_CMD_HID_INPUT      0x0Eu
#define IOC_RSP_HID_INPUT      0x8Eu
#define IOC_OFF_HID_INPUT_MAX  0u  /* request DATA 0: wanted byte count */
#define IOC_HID_INPUT_META_LEN 2u  /* reply DATA: queued, dropped       */
#define IOC_HID_INPUT_MAX_DATA 24u /* reply DATA: most translated bytes */

#endif /* ZEPHYROS_IOC_PROTOCOL_H */
