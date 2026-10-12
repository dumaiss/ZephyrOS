/* ZephyrOS libzos: ZEX v2 segment wrappers (packages 44a/44b).
 *
 * The argument blocks are marshalled through the shared low-window
 * scratch union (zos_internal.h) and the raw gate stub
 * (user/sdk/src/ipcgate.asm), exactly like the extended-memory family.
 * `farcall` is the far-call helper (architecture 36.17): map the
 * segment, write the token to the memory-control port, make the plain
 * CALL and restore the previous latch.
 */

#include "zos.h"
#include "zos_internal.h"

int seginfo(unsigned int segment, struct zos_seg_info *out)
{
	if (segment > 0xFFu || out == (struct zos_seg_info *)0) {
		return -ZOS_EINVAL;
	}
	zos_scratch.seg_info.segment = (unsigned char)segment;
	zos_scratch.seg_info.reserved = 0u;
	zos_scratch.seg_info.buffer = (unsigned int)out;
	return zos_ipc_raw(ZOS_SYSCALL_SEG_INFO, &zos_scratch.seg_info);
}

int segmap(unsigned int segment)
{
	if (segment > 0xFFu) {
		return -ZOS_EINVAL;
	}
	zos_scratch.seg_map.segment = (unsigned char)segment;
	zos_scratch.seg_map.reserved = 0u;
	return zos_ipc_raw(ZOS_SYSCALL_SEG_MAP, &zos_scratch.seg_map);
}

int farcall(unsigned int segment, zos_far_fn entry, int a, int b)
{
	int token;
	unsigned char saved;

	if (entry == (zos_far_fn)0) {
		return -ZOS_EINVAL;
	}
	token = segmap(segment);
	if (token < 0) {
		return token;
	}
	saved = aperture_open((unsigned char)token);
	a = entry(a, b);
	aperture_close(saved);
	return a;
}
