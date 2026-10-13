/* KonaOS native error codes (syscall-visible native ABI).
 *
 * This is the single source of every error value a native syscall can
 * return in A.  The kernel includes it as "kos/errno.h"; user programs
 * include it through the SDK (docs/sdk.md).  Errors that are never
 * syscall-visible (the IOC transport-class codes) live in the
 * syscall-visible (the IOC transport-class codes) live in the IOC class
 * header drivers/include/ioc.h.
 *
 * KOS_ENOSYS is what the dispatcher returns for an unassigned syscall
 * number.  The values are plain ints, so internal int-returning APIs
 * signal failure as the negated code (e.g. -KOS_EINVAL).  A syscall
 * returns the raw KOS_E* value in A with carry set; KOS_EFAULT is the
 * status the kernel synthesizes when a process entry returns through the
 * common trap instead of calling SYS_EXIT.
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
#define KOS_EFAULT    0x0C /* invalid process entry/exit fault */

/* Filesystem conditions (architecture 6.6).  KOS_EMFILE reports
 * handle-table and provider open-handle exhaustion, so it is never
 * confused with KOS_ENOSPC (storage full).  KOS_ENODEV (0x03) also means
 * "/fs has no media". */
#define KOS_EEXIST    0x10 /* object already exists               */
#define KOS_ENOTDIR   0x11 /* path component is not a directory   */
#define KOS_EISDIR    0x12 /* object is a directory               */
#define KOS_ENOTEMPTY 0x13 /* directory not empty                 */
#define KOS_EROFS     0x14 /* read-only filesystem                */
#define KOS_ESTALE    0x15 /* stale/removed object identity       */
#define KOS_EXDEV     0x16 /* cross-filesystem operation          */
#define KOS_EMFILE    0x17 /* descriptor/handle table exhausted   */

#endif /* KOS_ERRNO_H */
