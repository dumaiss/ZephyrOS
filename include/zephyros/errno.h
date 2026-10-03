/* ZephyrOS native error codes.
 *
 * M1 defines a single deterministic error: ZOS_ENOSYS, returned by the
 * native syscall dispatcher for every syscall number.  Later milestones
 * extend this file; the M1 value must stay stable.
 */

#ifndef ZEPHYROS_ERRNO_H
#define ZEPHYROS_ERRNO_H

#define ZOS_ENOSYS 0x01u /* syscall not implemented */

#endif /* ZEPHYROS_ERRNO_H */
