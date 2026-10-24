/* PView: the KIMG v1 header validation (see pview_kimg.h). */

#include "pview_kimg.h"

#include "kos/errno.h"

static unsigned int pview_u16(const uint8_t *at)
{
	return (unsigned int)at[0] | ((unsigned int)at[1] << 8);
}

static unsigned long pview_u32(const uint8_t *at)
{
	return (unsigned long)at[0] | ((unsigned long)at[1] << 8) |
	       ((unsigned long)at[2] << 16) | ((unsigned long)at[3] << 24);
}

int pview_kimg_check(const uint8_t *raw)
{
	unsigned int width;
	unsigned int height;
	unsigned int mode;
	unsigned long payload;

	if (raw == (const uint8_t *)0) {
		return -KOS_EINVAL;
	}
	if (raw[0] != 'K' || raw[1] != 'I' || raw[2] != 'M' ||
	    raw[3] != 'G' || raw[4] != 1u || raw[6] != 0u) {
		return -KOS_EINVAL;
	}
	mode = raw[5];
	if (mode > PVIEW_MODE_YJK) {
		return -KOS_EINVAL;
	}
	width = pview_u16(raw + 8);
	height = pview_u16(raw + 10);
	payload = pview_u32(raw + 12);
	if (width == 0u || height == 0u || height > 212u) {
		return -KOS_EINVAL;
	}
	if (mode == PVIEW_MODE_G6) {
		if ((width & 1u) != 0u || width > 512u || raw[7] > 16u) {
			return -KOS_EINVAL;
		}
		if (payload != (unsigned long)(width / 2u) * height) {
			return -KOS_EINVAL;
		}
	} else {
		if (width > 256u || raw[7] != 0u ||
		    (mode == PVIEW_MODE_YJK && (width & 3u) != 0u)) {
			return -KOS_EINVAL;
		}
		if (payload != (unsigned long)width * height) {
			return -KOS_EINVAL;
		}
	}
	return 0;
}
