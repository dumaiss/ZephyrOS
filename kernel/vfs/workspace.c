/* ZephyrOS VFS part: the staging buffer. */
/* ZephyrOS M9 bank-7 VFS workspace (package 27, correction-01).
 *
 * Owns the single 512-byte VFS workspace and, in debug/acceptance builds,
 * the guard that asserts its two views are never used together.  See
 * kernel/vfs/vfs.h for the view layout and the per-operation mapping.
 *
 * The workspace replaces the six separate pathname buffers and the staging
 * buffer package 27 first carried (about 0.94 KB of bank-7 runtime data,
 * two module-arena granules under the derived data base).  It is kernel
 * state: it holds caller path copies, normalized paths and file-transfer
 * chunks, and is never part of the native ABI.
 */

#include "kernel/vfs/vfs.h"
#include "kernel.h"

uint8_t vfs_workspace[VFS_WORKSPACE_SIZE];

#if defined(__SDCC)
_Static_assert(VFS_PATH_SLOTS * VFS_PATH_SLOT_SIZE == VFS_WORKSPACE_SIZE,
	       "the VFS workspace views must exactly tile the buffer");
#endif

#if ZOS_VFS_WS_GUARD
/* Which view currently owns the workspace (0 = free).  Execution is
 * single-tasking, so one byte is enough.  Entering a view while the other
 * one is still active would mean a live path pointer was overwritten by
 * staging data (or the reverse), so it is a fatal layout fault. */
uint8_t vfs_ws_view;

void vfs_ws_fault(void)
{
	panic("vfs: workspace view overlap");
}
#endif /* ZOS_VFS_WS_GUARD */
