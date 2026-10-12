/* ZephyrOS native error codes (syscall-visible native ABI).
 *
 * This is the single source of every error value a native syscall can
 * return in A.  The kernel includes it as "zos/errno.h"; user programs
 * include it through the SDK (docs/sdk.md).  Errors that are never
 * syscall-visible (the IOC transport-class codes) live in the
 * syscall-visible (the IOC transport-class codes) live in the IOC class
 * header drivers/include/ioc.h.
 *
 * M1 defines a single deterministic error: ZOS_ENOSYS, returned by the
 * native syscall dispatcher for every syscall number.  Later milestones
 * extend this file; the M1 value must stay stable.
 *
 * M3 adds generic subsystem error numbers.  They are plain int values so
 * that internal int-returning APIs can signal failure as the negated
 * code (e.g. -ZOS_EINVAL).  None of the M3
 * values are syscall-visible and the numeric values are internal.
 *
 * M6 makes a subset of these syscall-visible: the native syscall
 * dispatcher returns the raw ZOS_E* value in A with carry set, and
 * ZOS_EFAULT is the status the kernel synthesizes when a process entry
 * returns through the common trap instead of calling SYS_EXIT.
 */

#ifndef ZOS_ERRNO_H
#define ZOS_ERRNO_H

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
#define ZOS_EFAULT    0x0C /* invalid process entry/exit fault (M6) */

/* M9 makes the filesystem family syscall-visible (architecture section
 * 32.30, work/M9-REFERENCE.md).  ZOS_EMFILE reports descriptor-table and
 * provider open-handle exhaustion so it is never confused with
 * ZOS_ENOSPC (storage full).  ZOS_ENODEV (0x03) is reused for "a
 * filesystem at / is required but none is mounted". */
#define ZOS_EEXIST    0x10 /* object already exists               */
#define ZOS_ENOTDIR   0x11 /* path component is not a directory   */
#define ZOS_EISDIR    0x12 /* object is a directory               */
#define ZOS_ENOTEMPTY 0x13 /* directory not empty                 */
#define ZOS_EROFS     0x14 /* read-only filesystem                */
#define ZOS_ESTALE    0x15 /* stale/removed object identity       */
#define ZOS_EXDEV     0x16 /* cross-filesystem operation          */
#define ZOS_EMFILE    0x17 /* descriptor/handle table exhausted   */

#endif /* ZOS_ERRNO_H */
