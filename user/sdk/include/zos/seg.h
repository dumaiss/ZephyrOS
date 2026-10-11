/* ZephyrOS SDK: ZEX v2 segments (package 44a).
 *
 * The native call is abi/zos/seg.h (SYS_SEG_INFO).  A ZEX v2 `DATA`
 * segment is a loader-created extended-memory object owned by the
 * program: `zos_seg_info` returns its logical size, kind, load state and
 * the handle that abi/zos/xmem.h's read/write calls use.  A loader
 * segment is released only at exit, so `zos_xmem_free` refuses it.
 *
 * Package 44b adds `SYS_SEG_MAP` for `FAR_CODE` segments; this package's
 * loader refuses far-code containers.
 */

#ifndef ZOS_SDK_SEG_H
#define ZOS_SDK_SEG_H

#include "zos/types.h"
#include "zos/seg.h"

/* Describe container segment `segment` (1..count) into `out`.  Returns 0
 * or -errno (ZOS_EINVAL for an id outside the caller's own container or
 * a bad record pointer). */
int zos_seg_info(unsigned int segment, struct zos_seg_info *out);

#endif /* ZOS_SDK_SEG_H */
