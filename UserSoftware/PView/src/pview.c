/* PView: the KIMG v1 picture viewer (docs/gfx-sdk.md section 17).
 *
 *   pview picture.kimg
 *
 * The complete lifecycle: open the file, read and validate the header
 * (a bad one prints an error before the display is touched), take the
 * VDP, enter the image's mode, apply its palette, stream the payload
 * through libgfx's upload in 4 KiB blocks, show the display, wait for a
 * key and return the console.  The image is centered on the screen;
 * the 16-colour mode's x stays even.
 *
 * The program holds /dev/video exclusively, so its own lines are
 * printed outside the hold.
 */

#include "kos.h"
#include "kos/gfx.h"
#include "kos/kimg.h"

#include "pview_kimg.h"

#define PVIEW_BUFFER 4096u

static unsigned char pview_buffer[PVIEW_BUFFER];
static struct gfx_rgb pview_palette[16];

int main(int argc, char **argv)
{
	struct kimg_header header;
	struct gfx_info info;
	uint16_t x;
	uint16_t y;
	unsigned long left;
	unsigned int got;
	int file;
	int result;

	if (argc < 1) {
		printf("pview: usage: pview picture.kimg\r\n");
		return 1;
	}
	file = open(argv[0], KFS_O_READ);
	if (file < 0) {
		printf("pview: %s: cannot open %d\r\n", argv[0], file);
		return 1;
	}
	result = read(file, &header, sizeof(struct kimg_header));
	if (result < (int)sizeof(struct kimg_header) ||
	    pview_kimg_check((const uint8_t *)&header) < 0) {
		(void)close((unsigned int)file);
		printf("pview: %s: bad header\r\n", argv[0]);
		return 1;
	}
	result = gfx_open();
	if (result < 0) {
		(void)close((unsigned int)file);
		printf("pview: FAIL open %d\r\n", result);
		return 1;
	}
	result = gfx_set_mode((enum gfx_mode)header.mode);
	if (result < 0) {
		gfx_close();
		(void)close((unsigned int)file);
		printf("pview: FAIL mode %d\r\n", result);
		return 1;
	}
	if (header.palette_count != 0u) {
		unsigned int bytes = (unsigned int)header.palette_count * 3u;

		if (read(file, pview_palette, bytes) < (int)bytes ||
		    gfx_set_palette(0u, header.palette_count,
				    pview_palette) < 0) {
			gfx_close();
			(void)close((unsigned int)file);
			printf("pview: %s: bad palette\r\n", argv[0]);
			return 1;
		}
	}
	(void)gfx_get_info(&info);
	x = (uint16_t)((info.width - header.width) / 2u);
	if (header.mode == (uint8_t)GFX_MODE_512x212_16) {
		x = (uint16_t)(x & ~1u);
	}
	y = (uint16_t)((info.height - header.height) / 2u);
	result = gfx_upload_begin(x, y, header.width, header.height);
	if (result < 0) {
		gfx_close();
		(void)close((unsigned int)file);
		printf("pview: FAIL upload %d\r\n", result);
		return 1;
	}
	left = header.payload_size;
	while (left != 0u) {
		unsigned int chunk = (left > PVIEW_BUFFER) ? PVIEW_BUFFER
			: (unsigned int)left;

		got = read(file, pview_buffer, chunk);
		if (got < chunk) {
			gfx_close();
			(void)close((unsigned int)file);
			printf("pview: %s: short file\r\n", argv[0]);
			return 1;
		}
		result = gfx_upload_write(pview_buffer, (uint16_t)chunk);
		if (result < 0) {
			gfx_close();
			(void)close((unsigned int)file);
			printf("pview: FAIL write %d\r\n", result);
			return 1;
		}
		left -= chunk;
	}
	if (gfx_upload_end() != 0) {
		gfx_close();
		(void)close((unsigned int)file);
		printf("pview: FAIL end\r\n");
		return 1;
	}
	(void)gfx_display_enable(1u);
	(void)getchar(); /* the key: the only blocking point */
	gfx_close();
	(void)close((unsigned int)file);
	printf("pview: done\r\n");
	return 0;
}
