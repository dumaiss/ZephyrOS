/* KonaOS libkos: EXE v2 segment wrappers (packages 44a/44b/49d).
 *
 * The argument blocks are marshalled through the shared low-window
 * scratch union (kos_internal.h) and the raw gate stub
 * (user/sdk/src/ipcgate.asm), exactly like the extended-memory family.
 * `farcall` is the far-call helper (architecture 36.17): map the
 * segment, write the token to the memory-control port, make the plain
 * CALL and restore the previous latch.  `farmap`/`farunmap`/`blob`
 * (package 49d) are the far-data half: open the aperture onto a mappable
 * DATA segment so its linked C symbols become pointers, or hand back the
 * handle of a bundled `BLOBS` file for `xread`.
 */

#include "kos.h"
#include "kos_internal.h"

/* How many `DATAn` segments `farmap` can name: the four the rule
 * generates (`DATA1=`..`DATA4=`). */
#define FARMAP_ORDINALS 4u

int seginfo(unsigned int segment, struct kos_seg_info *out)
{
	if (segment > 0xFFu || out == (struct kos_seg_info *)0) {
		return -KOS_EINVAL;
	}
	kos_scratch.seg_info.segment = (unsigned char)segment;
	kos_scratch.seg_info.reserved = 0u;
	kos_scratch.seg_info.buffer = (unsigned int)out;
	return kos_ipc_raw(KOS_SYSCALL_SEG_INFO, &kos_scratch.seg_info);
}

int segmap(unsigned int segment)
{
	if (segment > 0xFFu) {
		return -KOS_EINVAL;
	}
	kos_scratch.seg_map.segment = (unsigned char)segment;
	kos_scratch.seg_map.reserved = 0u;
	return kos_ipc_raw(KOS_SYSCALL_SEG_MAP, &kos_scratch.seg_map);
}

int farcall(unsigned int segment, kos_far_fn entry, int a, int b)
{
	int token;
	unsigned char saved;

	if (entry == (kos_far_fn)0) {
		return -KOS_EINVAL;
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

/* The handles `farmap` has found, one per `DATAn` segment (package 49d):
 * the first call of an ordinal scans the container's rows and every later
 * call reuses the handle.  Empty while 0 - a DATA segment's handle is at
 * least 1. */
static unsigned char farmap_handles[FARMAP_ORDINALS];

/* The handle of the n-th DATA segment of one kind (1-based, container id
 * order): `mappable` picks the `DATAn` rows, otherwise the `BLOBS` rows.
 * Returns the handle or -errno; an id past the container's rows ends the
 * scan. */
static int farmap_scan(unsigned int n, unsigned int mappable)
{
	struct kos_seg_info info;
	unsigned int seen = 0u;
	unsigned int id;

	if (n == 0u) {
		return -KOS_EINVAL;
	}
	for (id = 1u; id <= 0xFFu; ++id) {
		if (seginfo(id, &info) != 0) {
			break;
		}
		if (info.kind != KOS_SEG_KIND_DATA || info.handle == 0u ||
		    ((info.flags & KOS_XMEM_FLAG_MAPPABLE) != 0u) !=
			    (mappable != 0u)) {
			continue;
		}
		if (++seen == n) {
			return (int)info.handle;
		}
	}
	return -KOS_EINVAL;
}

void *farmap(unsigned int n, unsigned char *prev)
{
	unsigned int address = 0u;
	int handle;
	int token;

	if (prev == (unsigned char *)0) {
		return (void *)0;
	}
	*prev = 0u;
	if (n == 0u || n > FARMAP_ORDINALS) {
		*prev = (unsigned char)-KOS_EINVAL;
		return (void *)0;
	}
	handle = (int)farmap_handles[n - 1u];
	if (handle == 0) {
		handle = farmap_scan(n, 1u);
		if (handle < 0) {
			*prev = (unsigned char)handle;
			return (void *)0;
		}
		farmap_handles[n - 1u] = (unsigned char)handle;
	}
	token = xmap((unsigned int)handle, &address);
	if (token < 0) {
		*prev = (unsigned char)token;
		return (void *)0;
	}
	*prev = aperture_open((unsigned char)token);
	return (void *)address;
}

void farunmap(unsigned char prev)
{
	aperture_close(prev);
}

int blob(unsigned int n)
{
	return farmap_scan(n, 0u);
}
