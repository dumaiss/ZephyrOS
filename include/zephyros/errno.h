/* ZephyrOS native error codes.
 *
 * M1 defines a single deterministic error: ZOS_ENOSYS, returned by the
 * native syscall dispatcher for every syscall number.  Later milestones
 * extend this file; the M1 value must stay stable.
 *
 * M3 adds generic subsystem error numbers.  They are plain int values so
 * that internal int-returning APIs can signal failure as the negated
 * code (e.g. -ZOS_EINVAL); see ZDEV_E* in device.h.  None of these are
 * syscall-visible and the numeric values are internal.
 */

#ifndef ZEPHYROS_ERRNO_H
#define ZEPHYROS_ERRNO_H

#define ZOS_ENOSYS 0x01u /* syscall not implemented */

#define ZOS_EINVAL    0x02 /* invalid argument/descriptor */
#define ZOS_ENODEV    0x03 /* no device bound / device missing */
#define ZOS_EBUSY     0x04 /* resource already owned/duplicate */
#define ZOS_ENOENT    0x05 /* object not found */
#define ZOS_ENOSPC    0x06 /* registry/resource exhausted */
#define ZOS_ENOTSUP   0x07 /* operation not supported by the device */
#define ZOS_ETIMEDOUT 0x08 /* bounded poll expired */
#define ZOS_EAGAIN    0x09 /* no data available right now */
#define ZOS_EIO       0x0A /* corrupt/truncated/overlapping data */
#define ZOS_EINIT     0x0B /* module/driver initialization refused */

#endif /* ZEPHYROS_ERRNO_H */
