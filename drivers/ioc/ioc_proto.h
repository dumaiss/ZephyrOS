/* Private IOC transport constants (drivers/ioc).
 *
 * This is NOT part of the public device API (drivers/include/ioc.h) and
 * NOT a client contract.  It extends the client-visible driver
 * vocabulary in drivers/include/ioc_protocol.h with the constants only
 * the IOC driver (and the test-only IOC transport diagnostic) may see:
 * the explicit unknown-class reply, the bulk READY/DONE vocabulary and
 * directions, the frozen set of request classes the pinned firmware can
 * use to arm the bulk lane, and the controller status bytes.  Wire
 * framing, frame construction, CRC/sequence handling, lane setup, the
 * READY/DONE admission state, and bulk recovery all stay under
 * drivers/ioc/; no production driver other than the IOC driver includes
 * this header.  The firmware is never modified.
 */

#ifndef KONAOS_DRIVERS_IOC_PROTO_H
#define KONAOS_DRIVERS_IOC_PROTO_H

#include "ioc_protocol.h"

/* The deployed firmware answers `type | 80h`; RSP_UNKNOWN_COMMAND is the
 * explicit unknown-class reply and the only response outside that rule.
 * Only the transport itself and the test-only IOC transport diagnostic
 * decode it; the client-visible PING vocabulary is in ioc_protocol.h. */
#define IOC_RSP_UNKNOWN_CMD   0xFEu

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
 * pattern, read only); four arm a Z80-to-MCU transfer.  Acceptance must
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
#define IOC_CMD_FS2_READ_RUN    0x45u

/* Status byte of the explicit unknown-class reply (RSP_UNKNOWN_COMMAND,
 * 0xFE): the deployed firmware's only documented "class not served"
 * answer, and the only wire evidence that uniquely proves a driver is
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

#endif /* KONAOS_DRIVERS_IOC_PROTO_H */
