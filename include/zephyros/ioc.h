/* ZephyrOS IOC transport vocabulary (driver-facing, M10).
 *
 * The IOC driver (drivers/ioc/) implements the Z80-to-MCU two-lane
 * transport: the command/reply lane (SIO1/B) and the bulk lane (SIO1/A).
 * The protocol clients (`hid.c`, `fs/iocfat`) reach it through the IPC
 * protocol of abi/zos/protocol.h (class ZOS_CLASS_IOC) and see only the
 * native frame below; the rolling sequence and the wire CRC are
 * transport-private and never appear here.
 *
 * This header owns:
 *
 *   - `struct zioc_frame`, the native command/reply frame: TYPE, STATUS,
 *     DATA length and up to ZIOC_MAX_DATA (26) DATA bytes;
 *   - the bulk-phase limits (ZIOC_BULK_MAX);
 *   - the semantic status bits a client reads from the driver's status
 *     operation (ZIOC_STATUS_HID_PENDING).
 *
 * The operation sequence, the bulk phases and the MCU packet vocabulary
 * live in include/zephyros/ioc_protocol.h; the class contract text is
 * retired with the M8 device model (work package 42c).
 */

#ifndef ZEPHYROS_IOC_H
#define ZEPHYROS_IOC_H

#include "zos/types.h"
#include "zephyros/errno.h"

/* Maximum command DATA bytes carried by one zioc_frame / wire packet. */
#define ZIOC_MAX_DATA 26u

/* Longest bulk DATA payload carried by one bulk_read()/bulk_write(). */
#define ZIOC_BULK_MAX 512u

/* Packed frame size: TYPE + STATUS + LENGTH + DATA. */
#define ZIOC_FRAME_SIZE (3u + ZIOC_MAX_DATA)

/* Semantic status bits returned by the driver's status operation. */
#define ZIOC_STATUS_HID_PENDING 0x0001u

/* Transport results (see kernel/ipc/ipc.h for the call conventions). */
#define ZIOC_OK        0
#define ZIOC_EINVAL    (-ZOS_EINVAL)
#define ZIOC_EBUSY     (-ZOS_EBUSY)
#define ZIOC_ETIMEDOUT (-ZOS_ETIMEDOUT)
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

#if defined(__SDCC)
/* Pin the packed native frame layout so a compiler/flag drift cannot
 * silently break the wire contract. */
_Static_assert(sizeof(struct zioc_frame) == ZIOC_FRAME_SIZE,
	       "zioc_frame must be 29 packed bytes");
_Static_assert(__builtin_offsetof(struct zioc_frame, status) == 1,
	       "zioc_frame status offset");
_Static_assert(__builtin_offsetof(struct zioc_frame, length) == 2,
	       "zioc_frame length offset");
_Static_assert(__builtin_offsetof(struct zioc_frame, data) == 3,
	       "zioc_frame data offset");
#endif /* __SDCC */

#endif /* ZEPHYROS_IOC_H */
