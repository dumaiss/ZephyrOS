/* ZephyrOS internal error codes that are never syscall-visible.
 *
 * The syscall-visible native error codes are owned by the public ABI
 * header abi/zos/errno.h, which this header includes so kernel and module
 * code keeps one name space.  The transport-class codes below belong to
 * the native IOC class (include/zephyros/ioc.h): a peer frame that
 * violates the wire framing rules, a checksum mismatch, and a stale or
 * mismatched exchange sequence.  They are internal and are never returned
 * in A by the native syscall dispatcher.
 */

#ifndef ZEPHYROS_ERRNO_H
#define ZEPHYROS_ERRNO_H

#include "zos/errno.h"

/* M8 adds internal transport-class errors for the native IOC class. */
#define ZOS_EPROTO    0x0D /* peer framing/length/type violation */
#define ZOS_ECRC      0x0E /* checksum verification failed */
#define ZOS_EBADSEQ   0x0F /* wrong or stale exchange sequence */

#endif /* ZEPHYROS_ERRNO_H */
