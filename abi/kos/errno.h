/* KonaOS native error codes (syscall-visible native ABI).
 *
 * This is the single source of every error value a native syscall can
 * return in A.  The kernel includes it as "kos/errno.h"; user programs
 * include it through the SDK (docs/sdk.md).  Errors that are never
 * syscall-visible (the IOC transport-class codes) live in the
 * syscall-visible (the IOC transport-class codes) live in the IOC class
 * header drivers/include/ioc.h.
 *
 * M1 defines a single deterministic error: KOS_ENOSYS, returned by the
 * native syscall dispatcher for every syscall number.  Later milestones
 * extend this file; the M1 value must stay stable.
 *
 * M3 adds generic subsystem error numbers.  They are plain int values so
 * that internal int-returning APIs can signal failure as the negated
 * code (e.g. -KOS_EINVAL).  None of the M3
 * values are syscall-visible and the numeric values are internal.
 *
 * M6 makes a subset of these syscall-visible: the native syscall
 * dispatcher returns the raw KOS_E* value in A with carry set, and
 * KOS_EFAULT is the status the kernel synthesizes when a process entry
 * returns through the common trap instead of calling SYS_EXIT.
 */

#ifndef KOS_ERRNO_H
#define KOS_ERRNO_H

#define KOS_ENOSYS 0x01u /* syscall not implemented */

#define KOS_EINVAL    0x02 /* invalid argument/descriptor */
#define KOS_ENODEV    0x03 /* no device bound / device missing */
#define KOS_EBUSY     0x04 /* resource already owned/duplicate */
#define KOS_ENOENT    0x05 /* object not found */
#define KOS_ENOSPC    0x06 /* registry/resource exhausted */
#define KOS_ENOTSUP   0x07 /* operation not supported by the device */
#define KOS_ETIMEDOUT 0x08 /* bounded poll expired */
#define KOS_EAGAIN    0x09 /* no data available right now */
#define KOS_EIO       0x0A /* corrupt/truncated/overlapping data */
#define KOS_EINIT     0x0B /* module/driver initialization refused */
#define KOS_EFAULT    0x0C /* invalid process entry/exit fault (M6) */

/* M9 makes the filesystem family syscall-visible (architecture section
 * 32.30, work/M9-REFERENCE.md).  KOS_EMFILE reports descriptor-table and
 * provider open-handle exhaustion so it is never confused with
 * KOS_ENOSPC (storage full).  KOS_ENODEV (0x03) is reused for "a
 * filesystem at / is required but none is mounted". */
#define KOS_EEXIST    0x10 /* object already exists               */
#define KOS_ENOTDIR   0x11 /* path component is not a directory   */
#define KOS_EISDIR    0x12 /* object is a directory               */
#define KOS_ENOTEMPTY 0x13 /* directory not empty                 */
#define KOS_EROFS     0x14 /* read-only filesystem                */
#define KOS_ESTALE    0x15 /* stale/removed object identity       */
#define KOS_EXDEV     0x16 /* cross-filesystem operation          */
#define KOS_EMFILE    0x17 /* descriptor/handle table exhausted   */

#endif /* KOS_ERRNO_H */
