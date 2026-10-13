/* KonaOS SDK: native file wrappers (the FILE and filesystem-namespace
 * protocols on typed handles).
 *
 * `open`/`close` are the familiar FILE words built on `bind`/`unbind`
 * (architecture 4.10): `open` binds the path, requires the FILE type
 * and returns a process-owned handle; `close` unbinds it.  The other
 * FILE operations are protocol calls on that handle.
 *
 * `stat`, `mkdir`, `rmdir`, `unlink` and `rename` are filesystem-namespace
 * operations (4.10, 6.2): they bind the owning provider root and send
 * the operation with the path; they act on names, not on an open file, and
 * there is no DIRECTORY handle.  A read-only provider answers `KOS_EROFS`.
 *
 * Strings are NUL-terminated; a string over 127 bytes (or an empty/null
 * one) is rejected locally with -KOS_EINVAL before any syscall, because
 * KOS_PATH_MAX counts the terminating NUL.  The kernel owns all window,
 * handle and path validation.
 *
 * Return convention matches kos/console.h: a non-negative result, or the
 * negated native error code.
 */

#ifndef KOS_SDK_FS_H
#define KOS_SDK_FS_H

#include "kos/types.h"
#include "kos/fs.h"
#include "kos/object.h"

/* Resolve `path` (flags = KFS_O_*) and bind a FILE instance.  Returns the
 * handle (>= 1) or the negated error; a path that resolves to something
 * other than a FILE is released and reported as -KOS_EINVAL. */
int open(const char *path, unsigned char flags);
int close(unsigned int handle);
int read(unsigned int handle, void *buffer, unsigned int length);
int write(unsigned int handle, const void *buffer, unsigned int length);
/* Returns the resulting 32-bit position (as a long) or -errno. */
long seek(unsigned int handle, long offset, unsigned char whence);
int sync(unsigned int handle);
/* Path metadata; `out` receives a `struct kfs_stat`. */
int stat(const char *path, struct kfs_stat *out);
int mkdir(const char *path);
int rmdir(const char *path);
int unlink(const char *path);
int rename(const char *oldpath, const char *newpath);

#endif /* KOS_SDK_FS_H */
