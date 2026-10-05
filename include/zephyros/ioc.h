/* ZephyrOS IOC class contract (M8, driver ABI v3).
 *
 * The IOC class owns the Z80-to-MCU two-lane transport: the command/reply
 * lane (SIO1/B) and the bulk lane (SIO1/A).  Class values, capability
 * bits, and the operation order are fixed by docs/devices.md and
 * docs/architecture.md section 30.6.
 *
 * This header is the only protocol surface ordinary clients see:
 *
 *   - `struct zioc_frame` is the native command/reply frame: TYPE,
 *     STATUS, DATA length, and up to ZIOC_MAX_DATA (26) DATA bytes.
 *     The rolling sequence and the wire CRC are transport-private and
 *     never appear here.
 *   - `struct zioc_ops.command()` maps one frame to the deployed MCU
 *     command packet, waits for the reply, and returns the validated
 *     reply frame.  On success the complete reply (type, status, length,
 *     and DATA bytes 0..length-1) is populated; DATA beyond the declared
 *     length is cleared.
 *   - A successful command reply to a class the controller can use to
 *     arm the bulk lane, shaped exactly like a bulk READY (status 0,
 *     length 8, nonzero transfer id, a known direction, and a length of
 *     1..512), admits one bulk phase.  The class gate matters: several
 *     immutable non-bulk replies are eight bytes long, and none of them
 *     may ever change the bulk phase.  While admitted, only
 *     `bulk_read()`/`bulk_write()` matching the READY direction and
 *     exact length are accepted; any other command fails with
 *     `ZIOC_EBUSY`.
 *   - `bulk_read()` completes and returns the lane to idle (a DONE
 *     query is optional for a read).  `bulk_write()` returning success
 *     means the bytes reached the MCU, not that they were stored: the
 *     instance then accepts only `command(CMD_XFER_STATUS)` and reports
 *     success only for a matching DONE record.  A failed or mismatched
 *     DONE is never reported as a completed write, and the next
 *     operation performs bounded whole-link recovery first.
 *   - `struct zioc_ops.status()` returns semantic status bits only,
 *     such as ZIOC_STATUS_HID_PENDING.  Raw SIO/`/CTSB`/RR0 bits are an
 *     IOC implementation detail and are never exposed.
 *
 * Transport results: zero is success; failures are the negative values
 * below.  ZIOC_EINVAL covers null/malformed arguments and an oversize
 * DATA length, ZIOC_EBUSY an overlapping/recursive use of the IOC
 * instance or an invalid bulk-phase sequence (no READY, wrong direction
 * or length, or a command other than the DONE query while a write is
 * awaiting it), ZIOC_EIO a hardware handshake fault or a controller
 * DONE status reporting that the transfer failed, and the remaining
 * codes describe bounded-wait expiry and peer-frame violations.
 *
 * The driver implements the pinned deployed host contract; consumers
 * never issue firmware changes to recover a different pairing.
 */

#ifndef ZEPHYROS_IOC_H
#define ZEPHYROS_IOC_H

#include "zephyros/types.h"
#include "zephyros/errno.h"
#include "zephyros/device.h"

/* Host-side transport contract implemented by this class: the pinned
 * deployed host level 08h.  Reported by the M8 target diagnostic; it is
 * a contract identifier, not a firmware request. */
#define ZIOC_TRANSPORT_LEVEL 0x08u

/* Maximum command DATA bytes carried by one zioc_frame / wire packet. */
#define ZIOC_MAX_DATA 26u

/* Longest bulk DATA payload carried by one bulk_read()/bulk_write(). */
#define ZIOC_BULK_MAX 512u

/* Packed frame size: TYPE + STATUS + LENGTH + DATA. */
#define ZIOC_FRAME_SIZE (3u + ZIOC_MAX_DATA)

/* Semantic status bits returned by zioc_ops.status(). */
#define ZIOC_STATUS_HID_PENDING 0x0001u

/* Transport results (see the header comment). */
#define ZIOC_OK        0
#define ZIOC_EINVAL    ZDEV_EINVAL
#define ZIOC_EBUSY     ZDEV_EBUSY
#define ZIOC_ETIMEDOUT ZDEV_ETIMEDOUT
#define ZIOC_EBADFRAME (-ZOS_EPROTO)
#define ZIOC_EBADCRC   (-ZOS_ECRC)
#define ZIOC_EBADSEQ   (-ZOS_EBADSEQ)
#define ZIOC_EIO       (-ZOS_EIO)

/* Native command/reply frame.  `length` is the number of valid DATA
 * bytes in a request, and the number of reply DATA bytes on success. */
struct zioc_frame {
	uint8_t type;
	uint8_t status;
	uint8_t length;
	uint8_t data[ZIOC_MAX_DATA];
};

/* Class operations, in the architecture's fixed order.  Either bulk
 * pointer may be null; the matching capability bit states whether the
 * operation is available. */
struct zioc_ops {
	int (*command)(void *state, const struct zioc_frame *request,
		       struct zioc_frame *reply);
	int (*bulk_read)(void *state, void *dst, uint16_t length);
	int (*bulk_write)(void *state, const void *src, uint16_t length);
	uint16_t (*status)(void *state);
};

#if defined(__SDCC)
/* Pin the packed native frame and operation table layouts so a
 * compiler/flag drift cannot silently break the class ABI. */
_Static_assert(sizeof(struct zioc_frame) == ZIOC_FRAME_SIZE,
	       "zioc_frame must be 29 packed bytes");
_Static_assert(__builtin_offsetof(struct zioc_frame, status) == 1,
	       "zioc_frame status offset");
_Static_assert(__builtin_offsetof(struct zioc_frame, length) == 2,
	       "zioc_frame length offset");
_Static_assert(__builtin_offsetof(struct zioc_frame, data) == 3,
	       "zioc_frame data offset");
_Static_assert(sizeof(struct zioc_ops) == 8,
	       "zioc_ops must be 8 packed bytes");
_Static_assert(__builtin_offsetof(struct zioc_ops, bulk_read) == 2,
	       "zioc_ops bulk_read offset");
_Static_assert(__builtin_offsetof(struct zioc_ops, bulk_write) == 4,
	       "zioc_ops bulk_write offset");
_Static_assert(__builtin_offsetof(struct zioc_ops, status) == 6,
	       "zioc_ops status offset");
#endif /* __SDCC */

#endif /* ZEPHYROS_IOC_H */
