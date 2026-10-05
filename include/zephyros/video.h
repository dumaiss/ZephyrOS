/* ZephyrOS VIDEO class contract (M8, driver ABI v3).
 *
 * The VIDEO class is the native graphics/video abstraction.  It carries
 * forward the useful semantic shape of the historical ZSDOS VIDEO_SEND
 * call -- operation + payload pointer + payload length -> status -- but
 * it is deliberately NOT a compatibility packet ABI: the operation
 * numbers below are native and must not be equated with the frozen
 * Virtual Drip packet numbering.  The later CP/M personality translates
 * that frozen interface to these native operations.
 *
 * The operation vocabulary is device-oriented.  A directly attached
 * video chip implements "write control/register bytes", "write VRAM
 * data bytes", "write palette bytes", "write indirect-register bytes",
 * "present" and "reset"; it does not implement packet types.
 *
 * Operations (exact native values):
 *
 *   ZVIDEO_CTRL_WRITE      1  stream payload bytes to the control port
 *   ZVIDEO_DATA_WRITE      2  stream payload bytes to the data port
 *   ZVIDEO_DATA_BLOCK      3  stream a block to the data port
 *   ZVIDEO_PALETTE_WRITE   4  stream payload bytes to the palette port
 *   ZVIDEO_INDIRECT_WRITE  5  stream payload bytes to the indirect port
 *   ZVIDEO_PRESENT         6  make prior writes visible (wait for the
 *                              command engine to finish)
 *   ZVIDEO_RESET           7  re-establish all driver-owned state
 *
 * The matching capability bits are allocated in device.h (bit range
 * 8-14, package 14): ZCAP_VIDEO_CTRL_WRITE, ZCAP_VIDEO_DATA_WRITE,
 * ZCAP_VIDEO_DATA_BLOCK, ZCAP_VIDEO_PALETTE, ZCAP_VIDEO_INDIRECT,
 * ZCAP_VIDEO_PRESENT, ZCAP_VIDEO_RESET.  Capabilities are
 * authoritative: callers select a device by class and capability, and
 * an operation whose capability is absent is reported as ZDEV_ENOTSUP
 * by the dispatcher rather than being inferred from the device name.
 *
 * Validation contract (a malformed request performs no I/O):
 *
 *   - write operations: `payload` must be non-null and `length` must be
 *     nonzero.  The full 16-bit length is streamed; native ZephyrOS
 *     does not inherit the historical Virtual Drip 16-byte non-block
 *     payload ceiling, which belongs to the old transport contract and
 *     not to a directly attached video chip.
 *   - ZVIDEO_PRESENT / ZVIDEO_RESET: `length` must be zero.
 *   - a null instance state reports ZDEV_ENODEV; a malformed request
 *     reports ZDEV_EINVAL; an unsupported operation reports
 *     ZDEV_ENOTSUP.
 *
 * `send` returns ZDEV_OK (0) or a negative ZDEV_Exxx error.  A
 * successful `send` means the operation was accepted and issued; for
 * streaming operations the bytes have been written to the chip (native
 * WAIT paces each access), and ZVIDEO_PRESENT orders command-engine
 * completion.
 */

#ifndef ZEPHYROS_VIDEO_H
#define ZEPHYROS_VIDEO_H

#include "zephyros/types.h"
#include "zephyros/errno.h"
#include "zephyros/device.h"

/* Native video operations (exact values; not Virtual Drip packet
 * numbers). */
#define ZVIDEO_CTRL_WRITE     1u
#define ZVIDEO_DATA_WRITE     2u
#define ZVIDEO_DATA_BLOCK     3u
#define ZVIDEO_PALETTE_WRITE  4u
#define ZVIDEO_INDIRECT_WRITE 5u
#define ZVIDEO_PRESENT        6u
#define ZVIDEO_RESET          7u

/* Class operations.  `state` is the opaque instance state returned by
 * device_interface(); `payload` is never dereferenced for zero-length
 * operations. */
struct zvideo_ops {
	int (*send)(void *state, uint8_t operation, const void *payload,
		    uint16_t length);
};

#if defined(__SDCC)
/* Pin the operation table layout so a compiler/flag drift cannot
 * silently break the class ABI. */
_Static_assert(sizeof(struct zvideo_ops) == 2,
	       "zvideo_ops must be 2 packed bytes");
#endif /* __SDCC */

#endif /* ZEPHYROS_VIDEO_H */
