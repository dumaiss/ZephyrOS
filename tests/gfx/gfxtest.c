/* KonaOS graphics foundation outcome program (card program, run from
 * the production shell by tests/test_gfx.py).
 *
 *   gfxtest info             DIRECT_INFO without the hold must be
 *                            -KOS_EBUSY; then the record through an
 *                            exclusive bind, printed after the release
 *   gfxtest mode6|mode7|yjk  open, set the mode, clear to a colour,
 *                            enable the display, wait for a key, close
 *   gfxtest <mode> leak      the same, returning without gfx_close and
 *                            without another line (one would be dropped
 *                            anyway): the process exit releases the
 *                            handle and must still restore the console
 *   gfxtest upload           four streaming uploads in chunks of 1, 7,
 *                            256 and 4,096 bytes, the short/overlong/
 *                            nested refusals, then the result codes
 *   gfxtest palette          G6 palette entries, a key, the G7 refusal,
 *                            a key, then the result code
 *
 * While /dev/video is held the console's output to the VDP is dropped,
 * so every line is printed outside the hold.
 */

#include "kos.h"
#include "kos/protocol.h"
#include "kos/gfx.h"

/* The clear colours: G6 takes a palette index, G7/YJK a pixel byte. */
#define GFXTEST_COLOR_G6  0x04u
#define GFXTEST_COLOR_G7  0x1Cu

/* The upload mode's transfer buffer (static: 4 KiB on the program
 * stack would exceed the reservation). */
#define GFXTEST_UPLOAD_CHUNK 4096u
static unsigned char gfxtest_upload_buffer[GFXTEST_UPLOAD_CHUNK];

/* The upload pattern (the tests compute the same bytes): the upload
 * mode runs GRAPHIC 6, where one payload byte packs two 4-bit
 * pixels. */
static unsigned char gfxtest_upload_byte(unsigned int i)
{
	unsigned char pixel = (unsigned char)((i * 7u + 5u) & 0x0Fu);

	return (unsigned char)((pixel << 4) | pixel);
}

static int gfxtest_direct_info(int video, struct kvideo_direct_info *info)
{
	struct kos_ipc_request req;
	int result;

	req.frame.op = KVIDEO_OP_DIRECT_INFO;
	req.frame.flags = 0u;
	req.frame.desc = KOS_IPC_DESC_NONE;
	req.frame.arg0 = 0u;
	req.frame.arg1 = 0u;
	req.buffer = (void *)0;
	req.length = 0u;
	req.direction = KOS_IPC_DESC_IN;
	result = call((unsigned char)video, &req);
	if (result < 0) {
		return result;
	}
	memcpy(info, req.frame.payload, sizeof(struct kvideo_direct_info));
	return 0;
}

static int gfxtest_info(void)
{
	struct kvideo_direct_info info;
	unsigned char type = 0u;
	int video;
	int result;

	video = bind("/dev/video", 0u, &type);
	if (video < 0) {
		printf("gfxtest: FAIL bind %d\r\n", video);
		return 1;
	}
	result = gfxtest_direct_info(video, &info);
	printf("gfxtest: refused %d\r\n", result);
	(void)unbind((unsigned char)video);

	video = bind("/dev/video", KOS_BIND_EXCLUSIVE, &type);
	if (video < 0) {
		printf("gfxtest: FAIL exclusive %d\r\n", video);
		return 1;
	}
	result = gfxtest_direct_info(video, &info);
	(void)unbind((unsigned char)video);
	if (result < 0) {
		printf("gfxtest: FAIL info %d\r\n", result);
		return 1;
	}
	printf("gfxtest: v%u b%u f%x r%u p%x %x %x %x %x c%x\r\n",
	       info.version, info.backend, info.flags, info.vram_kib,
	       info.data_port, info.command_port, info.palette_port,
	       info.indirect_port, info.config_port, info.config_initial);
	return 0;
}

static int gfxtest_run(const char *name, enum gfx_mode mode,
		       gfx_color_t color, int leak)
{
	struct gfx_info info;
	int result;

	printf("gfxtest: %s\r\n", name);
	result = gfx_open();
	if (result < 0) {
		printf("gfxtest: FAIL open %d\r\n", result);
		return 1;
	}
	result = gfx_set_mode(mode);
	if (result < 0) {
		gfx_close();
		printf("gfxtest: FAIL mode %d\r\n", result);
		return 1;
	}
	result = gfx_clear(color);
	if (result < 0) {
		gfx_close();
		printf("gfxtest: FAIL clear %d\r\n", result);
		return 1;
	}
	result = gfx_display_enable(1u);
	if (result < 0) {
		gfx_close();
		printf("gfxtest: FAIL enable %d\r\n", result);
		return 1;
	}
	result = gfx_get_info(&info);
	if (result < 0 || info.width == 0u ||
	    (mode == GFX_MODE_512x212_16 && info.width != 512u) ||
	    (mode != GFX_MODE_512x212_16 && info.width != 256u) ||
	    info.height != 212u) {
		gfx_close();
		printf("gfxtest: FAIL info %d\r\n", result);
		return 1;
	}
	(void)getchar(); /* the key: the only blocking point */
	if (leak == 0) {
		gfx_close();
		printf("gfxtest: done\r\n");
	}
	return 0;
}

/* One upload of `bytes` in chunks of `chunk`, then the short/overlong/
 * nested refusals; every failure record comes back through the result
 * flags and is printed after the release. */
static int gfxtest_upload(void)
{
	const unsigned int chunks[4] = {1u, 7u, 256u, 4096u};
	unsigned int run;
	int short_result = 0;
	int over_result = 0;
	int nest_result = 0;
	int result;

	result = gfx_open();
	if (result < 0) {
		printf("gfxtest: FAIL open %d\r\n", result);
		return 1;
	}
	result = gfx_set_mode(GFX_MODE_512x212_16);
	if (result < 0) {
		gfx_close();
		printf("gfxtest: FAIL mode %d\r\n", result);
		return 1;
	}
	/* Four rectangles, one per chunk size: a full-width off-screen
	 * page-1 strip, two partial rows on page 1, and a full-width
	 * page-0 strip exactly the size of the largest chunk. */
	for (run = 0u; run < 4u; ++run) {
		unsigned int at;
		unsigned int total;
		unsigned int done;
		unsigned int chunk = chunks[run];
		int r;

		switch (run) {
		case 0u:
			r = gfx_upload_begin(0u, 256u, 512u, 4u);
			break;
		case 1u:
			r = gfx_upload_begin(64u, 264u, 128u, 6u);
			break;
		case 2u:
			r = gfx_upload_begin(0u, 300u, 256u, 8u);
			break;
		default:
			r = gfx_upload_begin(0u, 0u, 512u, 16u);
			break;
		}
		if (r < 0) {
			gfx_close();
			printf("gfxtest: FAIL begin %d\r\n", r);
			return 1;
		}
		total = run == 3u ? 4096u : run == 0u ? 1024u :
			run == 1u ? 384u : 1024u;
		at = 0u;
		done = 0u;
		while (done < total) {
			unsigned int left = total - done;
			unsigned int piece = left < chunk ? left : chunk;
			unsigned int i;

			for (i = 0u; i < piece; ++i) {
				gfxtest_upload_buffer[i] = gfxtest_upload_byte(at + i);
			}
			r = gfx_upload_write(gfxtest_upload_buffer, (uint16_t)piece);
			if (r < 0) {
				gfx_close();
				printf("gfxtest: FAIL write %d\r\n", r);
				return 1;
			}
			at += piece;
			done += piece;
		}
		if (gfx_upload_end() != 0) {
			gfx_close();
			printf("gfxtest: FAIL end\r\n");
			return 1;
		}
	}
	/* A short stream: end with bytes missing refuses and cancels. */
	if (gfx_upload_begin(0u, 350u, 32u, 4u) == 0) {
		gfxtest_upload_buffer[0] = gfxtest_upload_byte(0u);
		(void)gfx_upload_write(gfxtest_upload_buffer, 20u);
		short_result = gfx_upload_end();
	}
	/* A stream past the end: the overlong chunk is refused whole. */
	if (gfx_upload_begin(0u, 360u, 32u, 4u) == 0) {
		unsigned int i;

		for (i = 0u; i < 64u; ++i) {
			gfxtest_upload_buffer[i] = gfxtest_upload_byte(i);
		}
		(void)gfx_upload_write(gfxtest_upload_buffer, 64u);
		over_result = gfx_upload_write(gfxtest_upload_buffer, 1u);
		(void)gfx_upload_end();
	}
	/* Nested begins refuse. */
	if (gfx_upload_begin(0u, 370u, 32u, 4u) == 0) {
		nest_result = gfx_upload_begin(0u, 380u, 32u, 4u);
		(void)gfx_upload_end();
	}
	gfx_close();
	printf("gfxtest: upload %d %d %d\r\n", short_result, over_result,
	       nest_result);
	if (short_result == -KOS_EINVAL && over_result == -KOS_EINVAL &&
	    nest_result == -KOS_EINVAL) {
		printf("gfxtest: upload PASS\r\n");
		return 0;
	}
	printf("gfxtest: upload FAIL\r\n");
	return 1;
}

/* The G6 palette entries, a key (the tests read the peer's palette
 * while the display is held), then the G7 refusal. */
static int gfxtest_palette(void)
{
	static const struct gfx_rgb colors[3] = {
		{0x00u, 0x00u, 0xFFu},
		{0xFFu, 0x00u, 0x00u},
		{0x00u, 0x80u, 0x00u}
	};
	int result;

	result = gfx_open();
	if (result < 0) {
		printf("gfxtest: FAIL open %d\r\n", result);
		return 1;
	}
	result = gfx_set_mode(GFX_MODE_512x212_16);
	if (result < 0) {
		gfx_close();
		printf("gfxtest: FAIL mode %d\r\n", result);
		return 1;
	}
	result = gfx_set_palette(1u, 3u, colors);
	if (result < 0) {
		gfx_close();
		printf("gfxtest: FAIL palette %d\r\n", result);
		return 1;
	}
	(void)getchar(); /* key 1: the palette is on the peer */
	result = gfx_set_mode(GFX_MODE_256x212_256);
	if (result < 0) {
		gfx_close();
		printf("gfxtest: FAIL mode %d\r\n", result);
		return 1;
	}
	result = gfx_set_palette(0u, 1u, colors);
	(void)getchar(); /* key 2 */
	gfx_close();
	printf("gfxtest: palette %d\r\n", result);
	if (result == -KOS_ENOTSUP) {
		printf("gfxtest: palette PASS\r\n");
		return 0;
	}
	printf("gfxtest: palette FAIL\r\n");
	return 1;
}

int main(int argc, char **argv)
{
	int leak;

	/* argv holds the command-tail words only (the kernel transports no
	 * program name), so argv[0] is the mode. */
	if (argc < 1) {
		printf("gfxtest: mode6|mode7|yjk|info|upload|palette [leak]"
		       "\r\n");
		return 1;
	}
	leak = argc > 1 && strcmp(argv[1], "leak") == 0;
	if (strcmp(argv[0], "info") == 0) {
		return gfxtest_info();
	}
	if (strcmp(argv[0], "upload") == 0) {
		return gfxtest_upload();
	}
	if (strcmp(argv[0], "palette") == 0) {
		return gfxtest_palette();
	}
	if (strcmp(argv[0], "mode6") == 0) {
		return gfxtest_run("mode6", GFX_MODE_512x212_16,
				   GFXTEST_COLOR_G6, leak);
	}
	if (strcmp(argv[0], "mode7") == 0) {
		return gfxtest_run("mode7", GFX_MODE_256x212_256,
				   GFXTEST_COLOR_G7, leak);
	}
	if (strcmp(argv[0], "yjk") == 0) {
		return gfxtest_run("yjk", GFX_MODE_256x212_YJK,
				   GFXTEST_COLOR_G7, leak);
	}
	printf("gfxtest: unknown mode\r\n");
	return 1;
}
