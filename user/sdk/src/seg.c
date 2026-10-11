/* ZephyrOS libzos: ZEX v2 segment wrapper (package 44a).
 *
 * The 4-byte argument block is marshalled through the shared low-window
 * scratch union (zos_internal.h) and the raw gate stub
 * (user/sdk/src/ipcgate.asm), exactly like the extended-memory family.
 */

#include "zos.h"
#include "zos_internal.h"

int zos_seg_info(unsigned int segment, struct zos_seg_info *out)
{
	if (segment > 0xFFu || out == (struct zos_seg_info *)0) {
		return -ZOS_EINVAL;
	}
	zos_scratch.seg_info.segment = (unsigned char)segment;
	zos_scratch.seg_info.reserved = 0u;
	zos_scratch.seg_info.buffer = (unsigned int)out;
	return zos_ipc_raw(ZOS_SYSCALL_SEG_INFO, &zos_scratch.seg_info);
}
