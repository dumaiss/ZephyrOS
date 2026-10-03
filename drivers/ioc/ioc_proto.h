/* Private MCU command-lane protocol constants (drivers/ioc).
 *
 * This is NOT part of the public device API (include/zephyros/ioc.h).
 * It pins the deployed Zephyr-80 IOC firmware vocabulary that the
 * ZephyrOS IOC driver and its protocol-aware clients speak: MCU command
 * and response types, the PING payload layout, and the pinned firmware
 * level of the deployment.  The firmware is never modified; the
 * constants document what it advertises.
 *
 * Ordinary clients use `struct zioc_frame` / `struct zioc_ops` and never
 * include this header.  Protocol-aware clients (the HID driver, storage,
 * and the M8 target diagnostics) include it to name command types and
 * decode payload offsets as native-frame DATA offsets.
 */

#ifndef ZEPHYROS_DRIVERS_IOC_PROTO_H
#define ZEPHYROS_DRIVERS_IOC_PROTO_H

/* MCU command classes (Z80 -> MCU) and their replies.  The deployed
 * firmware answers `type | 80h`; RSP_UNKNOWN_COMMAND is the explicit
 * unknown-class reply and the only response outside that rule. */
#define IOC_CMD_PING          0x01u
#define IOC_RSP_PING          0x81u
#define IOC_RSP_UNKNOWN_CMD   0xFEu

/* PING request/reply DATA layout (DATA offset = compatibility-mailbox
 * offset minus the four mailbox header bytes).  The request carries the
 * echo pattern; the reply echoes those bytes and appends diagnostics. */
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

/* Bulk READY / DONE lifecycle (mirrors the frozen ioc_frame.h of the
 * pinned deployment; the MCU firmware is never modified).
 *
 * A command that hands DATA to the bulk lane answers with a READY: a
 * successful reply whose 8-byte DATA carries the transfer id, direction,
 * and DATA length.  The following bulk packet is bound to that READY;
 * the id is echoed by the DONE record so a write can be confirmed.
 *
 * READY has no independent tag on the wire, and immutable successful
 * non-bulk replies are also eight bytes long (CMD_PROFILE page
 * IOC_PROFILE_PAGE_BULK_TX; CMD_HID_INPUT with six translated bytes,
 * whose `queued`/`dropped` header can satisfy every READY field check).
 * The driver therefore admits a bulk phase only for a reply to a request
 * class the pinned firmware can use to arm the bulk lane. */
#define IOC_CMD_XFER_STATUS     0x06u
#define IOC_RSP_XFER_STATUS     0x86u

#define IOC_READY_PAYLOAD_LEN   8u
#define IOC_OFF_READY_XFER_ID   0u
#define IOC_OFF_READY_DIRECTION 1u
#define IOC_OFF_READY_LEN_LO    2u
#define IOC_OFF_READY_LEN_HI    3u

#define IOC_BULK_DIR_MCU_TO_Z80 0x00u /* host reads  (READY -> bulk_read) */
#define IOC_BULK_DIR_Z80_TO_MCU 0x01u /* host writes (READY -> bulk_write) */

/* The complete frozen set of request classes that can arm the bulk lane.
 * Six arm a MCU-to-Z80 transfer (CMD_BULK_TEST is the diagnostic loopback
 * pattern, read only); four arm a Z80-to-MCU transfer.  M8 acceptance must
 * not invoke the diagnostic classes' raw/destructive paths; the classes
 * stay recognized because the pinned firmware can arm the lane with them. */
#define IOC_CMD_BULK_TEST       0x04u
#define IOC_CMD_SD_READ_BULK    0x05u
#define IOC_CMD_SD_WRITE_BULK   0x07u
#define IOC_CMD_SD_READ_REC     0x08u
#define IOC_CMD_SD_WRITE_REC    0x09u
#define IOC_CMD_SD_READ_BLOCK   0x0Fu
#define IOC_CMD_FS_READ         0x23u
#define IOC_CMD_FS_WRITE        0x24u
#define IOC_CMD_FS2_READ        0x36u
#define IOC_CMD_FS2_WRITE       0x3Eu

/* Status byte of the explicit unknown-class reply (RSP_UNKNOWN_COMMAND,
 * 0xFE): the deployed firmware's only documented "class not served"
 * answer, and the only wire evidence that uniquely proves a service is
 * absent.  A filesystem/media status never proves that. */
#define IOC_STATUS_UNKNOWN_CMD  0x02u

/* DONE record returned by CMD_XFER_STATUS.  Only the identity and the
 * final status are used by the driver; the pinned firmware appends
 * bring-up diagnostics that this driver ignores. */
#define IOC_OFF_DONE_XFER_ID    0u
#define IOC_OFF_DONE_STATUS     1u
#define IOC_DONE_MIN_DATA       2u

/* Controller status byte inside the DONE record. */
#define IOC_STATUS_OK           0x00u

#endif /* ZEPHYROS_DRIVERS_IOC_PROTO_H */
