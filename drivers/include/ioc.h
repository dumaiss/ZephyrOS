/* KonaOS IOC transport vocabulary (driver-facing).
 *
 * The IOC driver (drivers/ioc/) implements the Z80-to-MCU two-lane
 * transport: the command/reply lane (SIO1/B) and the bulk lane (SIO1/A).
 * The protocol clients (`hid.c`, `drivers/iocfat`) reach it through the IPC
 * protocol of abi/kos/protocol.h (class KOS_CLASS_IOC) and see only the
 * native frame below; the rolling sequence and the wire CRC are
 * transport-private and never appear here.
 *
 * This header owns:
 *
 *   - `struct kioc_frame`, the native command/reply frame: TYPE, STATUS,
 *     DATA length and up to KIOC_MAX_DATA (26) DATA bytes;
 *   - the bulk-phase limits (KIOC_BULK_MAX);
 *   - the semantic status bits a client reads from the driver's status
 *     operation (KIOC_STATUS_HID_PENDING).
 *
 * The operation sequence, the bulk phases and the MCU packet vocabulary
 * live in drivers/include/ioc_protocol.h.
 */

#ifndef KONAOS_IOC_H
#define KONAOS_IOC_H

#include "kos/types.h"
#include "kos/errno.h"

/* Transport-class errors of the native IOC class (T3 moved them here from
 * the deleted private errno header): a peer frame that violates the
 * wire framing rules, a checksum mismatch, and a stale or mismatched
 * exchange sequence.  They are internal and are never returned in A by
 * the native syscall dispatcher. */
#define KOS_EPROTO    0x0D /* peer framing/length/type violation */
#define KOS_ECRC      0x0E /* checksum verification failed */
#define KOS_EBADSEQ   0x0F /* wrong or stale exchange sequence */

/* Maximum command DATA bytes carried by one kioc_frame / wire packet. */
#define KIOC_MAX_DATA 26u

/* Longest bulk DATA payload carried by one bulk_read()/bulk_write(). */
#define KIOC_BULK_MAX 512u

/* Longest run one FS2_READ_RUN READY admits: one KIOC_BULK_MAX packet per
 * block, received back to back by KIOC_OP_COMMAND_READ. */
#define KIOC_RUN_MAX 8192u

/* Packed frame size: TYPE + STATUS + LENGTH + DATA. */
#define KIOC_FRAME_SIZE (3u + KIOC_MAX_DATA)

/* Semantic status bits returned by the driver's status operation. */
#define KIOC_STATUS_HID_PENDING 0x0001u

/* Transport results (see kernel/ipc/ipc.h for the call conventions). */
#define KIOC_OK        0
#define KIOC_EINVAL    (-KOS_EINVAL)
#define KIOC_EBUSY     (-KOS_EBUSY)
#define KIOC_ETIMEDOUT (-KOS_ETIMEDOUT)
#define KIOC_EBADFRAME (-KOS_EPROTO)
#define KIOC_EBADCRC   (-KOS_ECRC)
#define KIOC_EBADSEQ   (-KOS_EBADSEQ)
#define KIOC_EIO       (-KOS_EIO)

/* Native command/reply frame.  `length` is the number of valid DATA
 * bytes in a request, and the number of reply DATA bytes on success. */
struct kioc_frame {
	uint8_t type;
	uint8_t status;
	uint8_t length;
	uint8_t data[KIOC_MAX_DATA];
};

#if defined(__SDCC)
/* Pin the packed native frame layout so a compiler/flag drift cannot
 * silently break the wire contract. */
_Static_assert(sizeof(struct kioc_frame) == KIOC_FRAME_SIZE,
	       "kioc_frame must be 29 packed bytes");
_Static_assert(__builtin_offsetof(struct kioc_frame, status) == 1,
	       "kioc_frame status offset");
_Static_assert(__builtin_offsetof(struct kioc_frame, length) == 2,
	       "kioc_frame length offset");
_Static_assert(__builtin_offsetof(struct kioc_frame, data) == 3,
	       "kioc_frame data offset");
#endif /* __SDCC */

#endif /* KONAOS_IOC_H */
