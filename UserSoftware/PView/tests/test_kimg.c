/* PView's KIMG v1 header validator, host side: the same
 * pview_kimg_check the viewer runs on the target, over raw header
 * bytes.  Compiled and run by `make test` with the host's C compiler.
 */

#include <stdio.h>
#include <string.h>

#include "kos/errno.h"

#include "pview_kimg.h"

static int failures;

static void set_u16(uint8_t *at, unsigned int value)
{
	at[0] = (uint8_t)value;
	at[1] = (uint8_t)(value >> 8);
}

static void set_u32(uint8_t *at, unsigned long value)
{
	at[0] = (uint8_t)value;
	at[1] = (uint8_t)(value >> 8);
	at[2] = (uint8_t)(value >> 16);
	at[3] = (uint8_t)(value >> 24);
}

static void header(uint8_t raw[16], unsigned int mode, unsigned int width,
		   unsigned int height, unsigned int palette_count)
{
	memcpy(raw, "KIMG", 4);
	raw[4] = 1;
	raw[5] = (uint8_t)mode;
	raw[6] = 0;
	raw[7] = (uint8_t)palette_count;
	set_u16(raw + 8, width);
	set_u16(raw + 10, height);
	set_u32(raw + 12, mode == PVIEW_MODE_G6 ?
		(unsigned long)(width / 2) * height :
		(unsigned long)width * height);
}

static void expect(const uint8_t raw[16], int want)
{
	int got = pview_kimg_check(raw);

	if (got != want) {
		printf("FAIL header %02X%02X %u %u %u: got %d want %d\n",
		       raw[5], raw[7], raw[8], raw[10], (unsigned)raw[12],
		       got, want);
		++failures;
	}
}

int main(void)
{
	uint8_t raw[16];
	unsigned int width;

	header(raw, PVIEW_MODE_G6, 512, 212, 16);
	expect(raw, 0);
	header(raw, PVIEW_MODE_G6, 64, 32, 16);
	expect(raw, 0);
	header(raw, PVIEW_MODE_G7, 256, 212, 0);
	expect(raw, 0);
	header(raw, PVIEW_MODE_G7, 64, 32, 0);
	expect(raw, 0);
	header(raw, PVIEW_MODE_YJK, 64, 32, 0);
	expect(raw, 0);
	/* YJK runs are four pixels wide. */
	header(raw, PVIEW_MODE_YJK, 62, 32, 0);
	expect(raw, -KOS_EINVAL);

	/* Geometry bounds: the 16-colour mode to 512 even pixels, the
	 * byte modes to 256, every mode to 212 lines. */
	for (width = 2; width <= 520; width += 2) {
		header(raw, PVIEW_MODE_G6, width, 1, 0);
		expect(raw, width <= 512 ? 0 : -KOS_EINVAL);
	}
	for (width = 1; width <= 300; ++width) {
		header(raw, PVIEW_MODE_G7, width, 1, 0);
		expect(raw, width <= 256 ? 0 : -KOS_EINVAL);
	}
	for (width = 1; width <= 220; ++width) {
		header(raw, PVIEW_MODE_G7, 64, width, 0);
		expect(raw, width <= 212 ? 0 : -KOS_EINVAL);
	}

	header(raw, PVIEW_MODE_G6, 64, 32, 0);
	raw[0] = 'X';
	expect(raw, -KOS_EINVAL);
	header(raw, PVIEW_MODE_G6, 64, 32, 0);
	raw[4] = 2;
	expect(raw, -KOS_EINVAL);
	header(raw, PVIEW_MODE_G6, 64, 32, 0);
	raw[5] = 3;
	expect(raw, -KOS_EINVAL);
	header(raw, PVIEW_MODE_G6, 64, 32, 0);
	raw[6] = 1;
	expect(raw, -KOS_EINVAL);
	header(raw, PVIEW_MODE_G6, 64, 32, 0);
	raw[7] = 17;
	expect(raw, -KOS_EINVAL);
	header(raw, PVIEW_MODE_G6, 65, 32, 0);
	expect(raw, -KOS_EINVAL);
	header(raw, PVIEW_MODE_G6, 64, 213, 0);
	expect(raw, -KOS_EINVAL);
	header(raw, PVIEW_MODE_G7, 64, 32, 1);
	expect(raw, -KOS_EINVAL);
	header(raw, PVIEW_MODE_G6, 64, 32, 0);
	set_u32(raw + 12, 1025);
	expect(raw, -KOS_EINVAL);
	header(raw, PVIEW_MODE_G7, 64, 32, 0);
	set_u32(raw + 12, 2049);
	expect(raw, -KOS_EINVAL);

	if (failures != 0) {
		printf("test_kimg: %d failures\n", failures);
		return 1;
	}
	printf("test_kimg: PASS\n");
	return 0;
}
