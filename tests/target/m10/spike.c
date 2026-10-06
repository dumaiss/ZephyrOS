/* ZephyrOS M10 design spike: the ZEX v2 primary program (33b-33d).
 *
 * Packed as a ZEX v2 container with two auxiliary segments
 * (tests/m10/segments.json).  It exercises the resident v2 loader and the
 * M10 runtime: xmem allocation across a slot boundary, segment info, far
 * calls into both segments with an in/out frame, an ordinary syscall and
 * an FS_READ from far code, and a recursive far call that must return
 * ZOS_EBUSY.  The simulator replay also inspects the auxiliary bank and
 * the settled bank map; this program's own PASS/FAIL line is the
 * user-visible result.
 */

#include "zos.h"

static unsigned int checks;
static unsigned int failures;

static void check(const char *label, int ok)
{
	++checks;
	if (!ok) {
		++failures;
		zos_printf("m10 spike: FAIL %s\r\n", label);
	}
}

static unsigned int frame16(const uint8_t *frame)
{
	return (unsigned int)(frame[0] | ((unsigned int)frame[1] << 8));
}

static void put16(uint8_t *frame, unsigned int value)
{
	frame[0] = (unsigned char)(value & 0xFFu);
	frame[1] = (unsigned char)((value >> 8) & 0xFFu);
}

int main(int argc, char **argv)
{
	uint8_t frame[ZOS_FAR_FRAME_MAX];
	struct zos_xmem_info info;
	struct zos_zex_seg_info sinfo;
	unsigned int i;
	int handle;
	int rc;

	(void)argc;
	(void)argv;
	zos_printf("m10 spike: primary OK\r\n");

	/* xmem: one 9000-byte object spans two 8-KiB slots, so the write
	 * below crosses a slot boundary. */
	handle = zos_xmem_alloc(9000u);
	check("xmem-alloc", handle > 0);
	if (handle > 0) {
		uint8_t buf[8];

		for (i = 0; i < 8u; ++i) {
			buf[i] = (uint8_t)(0xA0u + i);
		}
		rc = zos_xmem_write((unsigned int)handle, 8188u, buf, 8u);
		check("xmem-write-boundary", rc == 8);
		for (i = 0; i < 8u; ++i) {
			buf[i] = 0;
		}
		rc = zos_xmem_read((unsigned int)handle, 8188u, buf, 8u);
		check("xmem-read-boundary", rc == 8);
		for (i = 0; i < 8u; ++i) {
			if (buf[i] != (uint8_t)(0xA0u + i)) {
				rc = -1;
			}
		}
		check("xmem-readback", rc == 8);
		rc = zos_xmem_read((unsigned int)handle, 0u, buf, 8u);
		check("xmem-zero", rc == 8 && buf[0] == 0 && buf[7] == 0);
		rc = zos_xmem_info((unsigned int)handle, &info);
		check("xmem-info",
		      rc == 8 && info.requested == 9000u &&
		      info.allocated >= 9000u &&
		      info.type == ZOS_XMEM_TYPE_RUNTIME);
		rc = zos_xmem_free((unsigned int)handle);
		check("xmem-free", rc == 0);
		rc = zos_xmem_read((unsigned int)handle, 0u, buf, 8u);
		check("xmem-stale", rc < 0);
	}

	/* segment info from the loader-built tables. */
	rc = zos_zex_segment_info(1u, &sinfo);
	check("seg1-info",
	      rc == 8 && sinfo.state == ZOS_XMEM_SEG_PRESENT &&
	      sinfo.image_size > 0);
	rc = zos_zex_segment_info(2u, &sinfo);
	check("seg2-info",
	      rc == 8 && sinfo.state == ZOS_XMEM_SEG_PRESENT &&
	      sinfo.image_size > 0);
	rc = zos_zex_segment_info(9u, &sinfo);
	check("seg-info-missing", rc < 0);

	/* far call into segment 1: sum plus its private BSS counter. */
	put16(frame, 0x1234u);
	put16(frame + 2, 0x0011u);
	rc = zos_far_call(1u, frame, 4u, frame, 2u);
	check("far-seg1", rc == 0x1245 && frame16(frame) == 0x1245u);

	/* far call into segment 2: index into its constant table. */
	frame[0] = 0u;
	rc = zos_far_call(2u, frame, 1u, frame, 2u);
	check("far-seg2", rc == 0x1111 && frame16(frame) == 0x1111u);

	/* an ordinary syscall from far code (console write from the aux
	 * bank; the kernel must copy from the execution bank). */
	rc = zos_far_call(3u, (const void *)0, 0u, frame, 2u);
	check("far-console", rc > 0 && (int)frame16(frame) == rc);

	/* an FS_READ from far code, on the recovery-store program itself. */
	rc = zos_open("/SYSTEM/spike.zex", ZFS_O_READ);
	check("far-open", rc >= 0);
	if (rc >= 0) {
		int fd = rc;

		put16(frame, (unsigned int)fd);
		rc = zos_far_call(4u, frame, 2u, frame, 4u);
		check("far-fsread",
		      rc == 8 && frame[0] == 'Z' && frame[1] == 'E' &&
		      frame[2] == 'X' && frame[3] == '2');
		(void)zos_close((unsigned int)fd);
	}

	/* a recursive far call is refused with ZOS_EBUSY. */
	rc = zos_far_call(5u, (const void *)0, 0u, frame, 2u);
	check("far-recursive-ebusy", rc == -ZOS_EBUSY);

	zos_printf("m10 spike: %u/%u checks\r\n", checks - failures, checks);
	if (failures == 0) {
		zos_printf("m10 spike: PASS\r\n");
		return 0;
	}
	zos_printf("m10 spike: FAIL\r\n");
	return 1;
}
