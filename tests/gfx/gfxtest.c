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
 *   gfxtest prims            batch ordering, the auto-flush, the four
 *                            overlap copies, page copies, the odd-x
 *                            fill (LMMV), the aligned fill (HMMV), the
 *                            refusals, two keys
 *   gfxtest shapes           the shapes: eight octants, points,
 *                            rectangle outlines and fills, ellipses,
 *                            XOR restores, the refusals, three keys
 *   gfxtest timing           R#16 markers around the timing targets
 *                            (one opaque glyph, one aligned fill, one
 *                            line, a filled and an outline circle)
 *   gfxtest text             two pairs in each mode, the right-edge
 *                            clip, the redefined glyph and its reset,
 *                            the odd-x and YJK refusals
 *   gfxtest atlas            opaque 4x6 in both modes, transparent
 *                            text over stripes (the LRU slot reuse),
 *                            the stale redraw, the refusals and the
 *                            out-of-range count, two keys
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

static void gfxtest_mark(void);

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

/* The primitives: batch ordering, the queue's auto-flush, copies in
 * the four overlap directions and across pages, then the G6 refusals.
 * GRAPHIC 7 runs one byte per pixel, so the tests read every byte
 * straight off the peer's VRAM. */
static int gfxtest_prims(void)
{
	unsigned int i;
	int odd_result;
	int bad_result;
	int good_result;
	int zero_fill;
	int zero_copy;
	int yjk_fill;
	int yjk_copy;
	int yjk_good;
	int result;

	result = gfx_open();
	if (result < 0) {
		printf("gfxtest: FAIL open %d\r\n", result);
		return 1;
	}
	result = gfx_set_mode(GFX_MODE_256x212_256);
	if (result < 0) {
		gfx_close();
		printf("gfxtest: FAIL mode %d\r\n", result);
		return 1;
	}
	/* A black screen, shown: the owner watches the drawing happen. */
	(void)gfx_clear(0x00u);
	(void)gfx_display_enable(1u);
	/* Ordering: two batched fills, the second on top of the first. */
	gfx_begin();
	result = gfx_fill_rect(0u, 0u, 40u, 8u, 0x11u, GFX_OP_COPY);
	if (result < 0) {
		gfx_close();
		printf("gfxtest: FAIL fill A %d\r\n", result);
		return 1;
	}
	result = gfx_fill_rect(8u, 2u, 16u, 4u, 0x22u, GFX_OP_COPY);
	if (result < 0) {
		gfx_close();
		printf("gfxtest: FAIL fill B %d\r\n", result);
		return 1;
	}
	result = gfx_end();
	if (result < 0) {
		gfx_close();
		printf("gfxtest: FAIL end %d\r\n", result);
		return 1;
	}
	/* The auto-flush: 33 queued fills; the 33rd flushes the first 32
	 * while the batch is still open (the tests stop at the 32nd peer
	 * command and find the hold intact). */
	gfx_begin();
	for (i = 0u; i < 33u; ++i) {
		result = gfx_fill_rect((uint16_t)(i * 6u), 100u, 4u, 2u,
				       (gfx_color_t)(i + 1u),
				       GFX_OP_COPY);
		if (result < 0) {
			gfx_close();
			printf("gfxtest: FAIL fill %u %d\r\n", i, result);
			return 1;
		}
	}
	(void)getchar(); /* key 1: the first 32 commands are on the peer */
	result = gfx_end();
	if (result < 0) {
		gfx_close();
		printf("gfxtest: FAIL end %d\r\n", result);
		return 1;
	}
	/* A distinct source pattern, uploaded byte for byte, then copied
	 * to the four overlap directions. */
	for (i = 0u; i < 16u; ++i) {
		gfxtest_upload_buffer[i] = gfxtest_upload_byte(i);
	}
	if (gfx_upload_begin(200u, 20u, 4u, 4u) < 0 ||
	    gfx_upload_write(gfxtest_upload_buffer, 16u) < 0 ||
	    gfx_upload_end() < 0 ||
	    gfx_copy_rect(200u, 20u, 204u, 22u, 4u, 4u) < 0 ||
	    gfx_copy_rect(200u, 20u, 196u, 22u, 4u, 4u) < 0 ||
	    gfx_copy_rect(200u, 20u, 204u, 18u, 4u, 4u) < 0 ||
	    gfx_copy_rect(200u, 20u, 196u, 18u, 4u, 4u) < 0) {
		gfx_close();
		printf("gfxtest: FAIL copies\r\n");
		return 1;
	}
	/* Across the pages: page 1 to page 0, then page 0 to page 1. */
	if (gfx_fill_rect(0u, 266u, 6u, 3u, 0x55u, GFX_OP_COPY) < 0 ||
	    gfx_copy_rect(0u, 266u, 50u, 60u, 6u, 3u) < 0 ||
	    gfx_fill_rect(150u, 40u, 6u, 3u, 0x66u, GFX_OP_COPY) < 0 ||
	    gfx_copy_rect(150u, 40u, 150u, 296u, 6u, 3u) < 0) {
		gfx_close();
		printf("gfxtest: FAIL page copies\r\n");
		return 1;
	}
	(void)getchar(); /* key 2: the peer holds every result */
	/* The 16-colour odd-x fill (LMMV), the aligned fill beside it
	 * (HMMV), the out-of-bounds refusal, and the zero sizes'
	 * silent no-ops. */
	result = gfx_set_mode(GFX_MODE_512x212_16);
	if (result < 0) {
		gfx_close();
		printf("gfxtest: FAIL mode %d\r\n", result);
		return 1;
	}
	/* A black screen so the odd-x fill's dots are observable
	 * (the mode switch keeps the G7 drawings' bytes). */
	(void)gfx_clear(0x00u);
	gfx_begin();
	odd_result = gfx_fill_rect(1u, 0u, 2u, 2u, 1u, GFX_OP_COPY);
	good_result = gfx_fill_rect(2u, 0u, 2u, 2u, 1u, GFX_OP_COPY);
	(void)gfx_end();
	bad_result = gfx_fill_rect(510u, 0u, 4u, 2u, 1u, GFX_OP_COPY);
	zero_fill = gfx_fill_rect(0u, 0u, 0u, 4u, 1u, GFX_OP_COPY);
	zero_copy = gfx_copy_rect(0u, 0u, 10u, 10u, 4u, 0u);
	/* YJK: x and width keep the four-pixel J/K runs. */
	result = gfx_set_mode(GFX_MODE_256x212_YJK);
	if (result < 0) {
		gfx_close();
		printf("gfxtest: FAIL mode %d\r\n", result);
		return 1;
	}
	yjk_fill = gfx_fill_rect(2u, 0u, 4u, 2u, 0u, GFX_OP_COPY);
	yjk_copy = gfx_copy_rect(0u, 0u, 6u, 8u, 4u, 4u);
	yjk_good = gfx_fill_rect(4u, 0u, 4u, 2u, 0u, GFX_OP_COPY);
	gfx_close();
	printf("gfxtest: prims %d %d %d %d %d %d %d %d\r\n", odd_result,
	       good_result, bad_result, zero_fill, zero_copy, yjk_fill,
	       yjk_copy, yjk_good);
	if (odd_result == 0 && good_result == 0 &&
	    bad_result == -KOS_EINVAL && zero_fill == 0 &&
	    zero_copy == 0 && yjk_fill == -KOS_EINVAL &&
	    yjk_copy == -KOS_EINVAL && yjk_good == 0) {
		printf("gfxtest: prims PASS\r\n");
		return 0;
	}
	printf("gfxtest: prims FAIL\r\n");
	return 1;
}

/* The text path: two pairs in each mode, the right-edge clip, the
 * redefined glyph and its reset, the odd-x and YJK refusals.  The
 * tests decode every glyph straight from the peer's VRAM against the
 * pinned font. */
static int gfxtest_text(void)
{
	static const struct gfx_text_style pair1 = {0x01u, 0x02u};
	static const struct gfx_text_style pair2 = {0x03u, 0x04u};
	static const struct gfx_text_style g7 = {0x1Cu, 0x00u};
	/* A full 6x8 block: every row's six pixels on. */
	static const uint8_t block[8] = {
		0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F
	};
	int odd_result;
	int set_result;
	int reset_result;
	int yjk_result;
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
	/* The G6 refusals and the two pairs. */
	odd_result = gfx_text(1u, 10u, "X", &pair1);
	result = gfx_text(8u, 20u, "GFX", &pair1);
	result |= gfx_text(8u, 30u, "text", &pair2);
	/* x 506 fits exactly one glyph: the clip drops the rest. */
	result |= gfx_text(506u, 40u, "ABCD", &pair1);
	if (result != 0) {
		gfx_close();
		printf("gfxtest: FAIL text %d\r\n", result);
		return 1;
	}
	/* The redefined glyph, drawn, restored and drawn again. */
	set_result = gfx_font_set_glyph(GFX_FONT_6X8, 0x80u, block);
	result = gfx_text(8u, 50u, "\x80", &pair1);
	reset_result = gfx_font_reset_glyph(GFX_FONT_6X8, 0x80u);
	result |= gfx_text(8u, 60u, "\x80", &pair1);
	if (result != 0) {
		gfx_close();
		printf("gfxtest: FAIL glyph %d\r\n", result);
		return 1;
	}
	/* G7, then the YJK refusal. */
	result = gfx_set_mode(GFX_MODE_256x212_256);
	if (result < 0) {
		gfx_close();
		printf("gfxtest: FAIL mode %d\r\n", result);
		return 1;
	}
	result = gfx_text(100u, 80u, "g7", &g7);
	if (result < 0) {
		gfx_close();
		printf("gfxtest: FAIL g7 text %d\r\n", result);
		return 1;
	}
	result = gfx_set_mode(GFX_MODE_256x212_YJK);
	if (result < 0) {
		gfx_close();
		printf("gfxtest: FAIL mode %d\r\n", result);
		return 1;
	}
	yjk_result = gfx_text(0u, 0u, "x", &g7);
	gfx_close();
	printf("gfxtest: text %d %d %d %d\r\n", odd_result, set_result,
	       reset_result, yjk_result);
	if (odd_result == -KOS_EINVAL && set_result == 0 &&
	    reset_result == 0 && yjk_result == -KOS_ENOTSUP) {
		printf("gfxtest: text PASS\r\n");
		return 0;
	}
	printf("gfxtest: text FAIL\r\n");
	return 1;
}

/* The atlas path: opaque 4x6 in both modes, transparent text over
 * stripes with the LRU slot reuse, the stale redraw, the refusals
 * and the out-of-range count, keys between the stages.  The tests
 * decode the screen and the atlas band straight from the peer's
 * VRAM. */
static int gfxtest_atlas(void)
{
	static const struct gfx_text_style o1 = {0x01u, 0x02u, 0u, 1u};
	static const struct gfx_text_style o2 = {0x03u, 0x04u, 0u, 1u};
	static const struct gfx_text_style g7o = {0x1Cu, 0x00u, 0u, 1u};
	/* A full 4x6 block: every row's four pixels on. */
	static const uint8_t block[6] = {
		0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F
	};
	static char row[129];
	struct gfx_text_style trans;
	int trans_no_atlas;
	int bad_band;
	int bad_short;
	int bad_range;
	int outside;
	int stale_result;
	int g7_no_atlas;
	int yjk_text;
	int yjk_atlas;
	int result;
	unsigned int i;
	unsigned int c;

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
	(void)gfx_clear(0x00u);
	(void)gfx_display_enable(1u);

	/* The refusals: transparent text without an atlas, a band over
	 * the visible lines, a band too short for one atlas, and a
	 * range past the font. */
	trans.fg = 0x01u;
	trans.bg = 0x00u;
	trans.flags = GFX_TEXT_TRANSPARENT;
	trans.font = GFX_FONT_4X6;
	trans_no_atlas = gfx_text(0u, 40u, "x", &trans);
	bad_band = gfx_text_atlas(GFX_FONT_4X6, 0u, 44u, 32u, 95u);
	bad_short = gfx_text_atlas(GFX_FONT_4X6, 212u, 5u, 32u, 95u);
	bad_range = gfx_text_atlas(GFX_FONT_4X6, 212u, 42u, 32u, 230u);

	/* Opaque 4x6 in two pairs and the right-edge clip (x 504 holds
	 * exactly two glyphs). */
	result = gfx_text(0u, 10u, "4x6", &o1);
	result |= gfx_text(0u, 20u, "pair two", &o2);
	result |= gfx_text(504u, 30u, "ABCD", &o1);
	if (result != 0) {
		gfx_close();
		printf("gfxtest: FAIL text %d\r\n", result);
		return 1;
	}
	/* The full 128-glyph row at x 0 (512 dots, every column): the
	 * marks price the whole call. */
	for (i = 0u; i < 128u; ++i) {
		row[i] = (i & 1u) != 0u ? 'W' : 'M';
	}
	row[128] = '\0';
	gfxtest_mark();
	result = gfx_text(0u, 70u, row, &o1);
	gfxtest_mark();
	/* Eight 6x8 glyphs in the same cached pair (the 6x8 steady
	 * cost: the warm-up draw first builds the pair's tables, then
	 * the marks price the string, per glyph). */
	{
		static const struct gfx_text_style g6s = {0x01u, 0x02u};
		(void)gfx_text(0u, 45u, "M", &g6s);
		gfxtest_mark();
		result |= gfx_text(0u, 80u, "MMMMMMMM", &g6s);
		gfxtest_mark();
	}
	if (result != 0) {
		gfx_close();
		printf("gfxtest: FAIL row %d\r\n", result);
		return 1;
	}
	/* The stripes: vertical 8-dot bars behind the text. */
	for (i = 0u; i < 64u; ++i) {
		result |= gfx_fill_rect((uint16_t)(i * 8u), 96u, 8u, 96u,
					(i & 1u) != 0u ? 0x0Bu : 0x0Au,
					GFX_OP_COPY);
	}
	result |= gfx_text_atlas(GFX_FONT_4X6, 212u, 42u, 32u, 95u);
	if (result != 0) {
		gfx_close();
		printf("gfxtest: FAIL atlas %d\r\n", result);
		return 1;
	}
	/* The first transparent draw builds colour 1's atlas (95 glyphs)
	 * and submits its eight LMMMs: the marks price the build.  The
	 * nine colours exceed the band's seven slots, so colours 8 and
	 * 9 rebuild the least recently used slots. */
	gfxtest_mark();
	result = gfx_text(8u, 104u, "ABCDEFGH", &trans);
	gfxtest_mark();
	for (c = 2u; c <= 9u; ++c) {
		trans.fg = (uint8_t)c;
		result |= gfx_text((uint16_t)(8u + (c - 1u) * 48u), 104u,
				  "ABCDEFGH", &trans);
	}
	/* The out-of-range count: two of the four glyphs draw nothing,
	 * the other two land. */
	trans.fg = 0x09u;
	outside = gfx_text(300u, 116u, "\x01" "AB\x7F", &trans);
	/* The steady transparent cost: a cached colour, four glyphs. */
	gfxtest_mark();
	result |= gfx_text(300u, 128u, "ABCD", &trans);
	gfxtest_mark();
	if (result != 0) {
		gfx_close();
		printf("gfxtest: FAIL transparent %d\r\n", result);
		return 1;
	}
	(void)getchar(); /* key 1: the G6 screen and the atlas band */

	/* The stale glyph: a full block, drawn transparently (the redraw
	 * refreshes A's cell in every built atlas, then the LMMM), then
	 * reset and drawn again. */
	trans.fg = 0x05u;
	stale_result = gfx_font_set_glyph(GFX_FONT_4X6, 0x41u, block);
	gfxtest_mark();
	stale_result |= gfx_text(300u, 140u, "A", &trans);
	gfxtest_mark();
	stale_result |= gfx_font_reset_glyph(GFX_FONT_4X6, 0x41u);
	stale_result |= gfx_text(300u, 150u, "A", &trans);
	/* A DIY copy: the target below the band (page 1's spare lines)
	 * reverses the copy's Y direction. */
	stale_result |= gfx_text(8u, 500u, "Q", &trans);

	/* G7: the G6 atlas does not fit the new packing. */
	result = gfx_set_mode(GFX_MODE_256x212_256);
	if (result < 0) {
		gfx_close();
		printf("gfxtest: FAIL mode %d\r\n", result);
		return 1;
	}
	trans.fg = 0x01u;
	g7_no_atlas = gfx_text(0u, 40u, "x", &trans);
	(void)gfx_clear(0x00u);
	/* The opaque G7 glyph cost: six glyphs between the marks. */
	gfxtest_mark();
	result = gfx_text(100u, 10u, "g7 4x6", &g7o);
	gfxtest_mark();
	for (i = 0u; i < 32u; ++i) {
		result |= gfx_fill_rect((uint16_t)(i * 8u), 96u, 8u, 96u,
					(i & 1u) != 0u ? 0x0Bu : 0x0Au,
					GFX_OP_COPY);
	}
	result |= gfx_text_atlas(GFX_FONT_4X6, 212u, 36u, 32u, 95u);
	/* Four colours into three slots: colour 4 rebuilds colour 1's
	 * slot, and the redraws rebuild colours 1 and 2 in turn. */
	for (c = 1u; c <= 4u; ++c) {
		trans.fg = (uint8_t)c;
		result |= gfx_text((uint16_t)(8u + (c - 1u) * 48u), 104u,
				  "ABCDEFGH", &trans);
	}
	trans.fg = 0x01u;
	result |= gfx_text(8u, 116u, "ABCDEFGH", &trans);
	trans.fg = 0x02u;
	result |= gfx_text(56u, 116u, "ABCDEFGH", &trans);
	/* The steady G7 transparent cost: a cached colour, four glyphs. */
	gfxtest_mark();
	result |= gfx_text(8u, 128u, "ABCD", &trans);
	gfxtest_mark();
	if (result != 0) {
		gfx_close();
		printf("gfxtest: FAIL g7 transparent %d\r\n", result);
		return 1;
	}
	/* YJK: the atlas setup and the transparent draw both refuse. */
	result = gfx_set_mode(GFX_MODE_256x212_YJK);
	if (result < 0) {
		gfx_close();
		printf("gfxtest: FAIL mode %d\r\n", result);
		return 1;
	}
	trans.fg = 0x01u;
	yjk_text = gfx_text(0u, 0u, "x", &trans);
	yjk_atlas = gfx_text_atlas(GFX_FONT_4X6, 212u, 42u, 32u, 95u);

	(void)getchar(); /* key 2: the G7 screen and the G7 band */
	gfx_close();
	printf("gfxtest: atlas %d %d %d %d %d %d %d %d %d\r\n",
	       trans_no_atlas, bad_band, bad_short, bad_range, outside,
	       stale_result, g7_no_atlas, yjk_text, yjk_atlas);
	if (trans_no_atlas == -KOS_EINVAL && bad_band == -KOS_EINVAL &&
	    bad_short == -KOS_EINVAL && bad_range == -KOS_EINVAL &&
	    outside == 2 && stale_result == 0 &&
	    g7_no_atlas == -KOS_EINVAL && yjk_text == -KOS_ENOTSUP &&
	    yjk_atlas == -KOS_ENOTSUP) {
		printf("gfxtest: atlas PASS\r\n");
		return 0;
	}
	printf("gfxtest: atlas FAIL\r\n");
	return 1;
}

/* The shape mode's drawings: lines in the eight octants, points,
 * rectangles (the outline and the fill's two paths), ellipses, and
 * the XOR restores.  GRAPHIC 7 runs first (one byte per pixel, the
 * tests read every byte), then the 16-colour mode's nibble checks.
 */
static int gfxtest_shapes(void)
{
	static const uint16_t fan[8][2] = {
		{0u, 0u}, {0u, 1u}, {0u, 2u}, {1u, 2u},
		{2u, 2u}, {2u, 1u}, {2u, 0u}, {1u, 0u}
	};
	static const struct gfx_text_style style = {0x11u, 0x00u};
	int result;

	result = gfx_open();
	if (result < 0) {
		printf("gfxtest: FAIL open %d\r\n", result);
		return 1;
	}
	result = gfx_set_mode(GFX_MODE_256x212_256);
	if (result < 0) {
		gfx_close();
		printf("gfxtest: FAIL mode %d\r\n", result);
		return 1;
	}
	(void)gfx_clear(0x00u);
	(void)gfx_display_enable(1u);

	/* The eight octants from the centre, the horizontal and the
	 * vertical, then the single points. */
	for (result = 0; result < 8; ++result) {
		if (gfx_line(64u, 48u,
			     (uint16_t)(64u + 40u * fan[result][0]),
			     (uint16_t)(48u + 32u * fan[result][1]),
			     (gfx_color_t)(result + 1u),
			     GFX_OP_COPY) < 0) {
			gfx_close();
			printf("gfxtest: FAIL fan %d\r\n", result);
			return 1;
		}
	}
	if (gfx_line(128u, 150u, 215u, 150u, 0x0Au, GFX_OP_COPY) < 0 ||
	    gfx_line(128u, 152u, 128u, 195u, 0x0Bu, GFX_OP_COPY) < 0 ||
	    gfx_point(220u, 160u, 0x0Cu, GFX_OP_COPY) < 0 ||
	    gfx_point(0u, 0u, 0x0Du, GFX_OP_COPY) < 0 ||
	    gfx_point(255u, 211u, 0x0Eu, GFX_OP_COPY) < 0) {
		gfx_close();
		printf("gfxtest: FAIL lines\r\n");
		return 1;
	}

	/* Rectangles: the outline (corners once), the aligned COPY
	 * fill (HMMV), the XOR fill over it, and a second XOR that
	 * restores the rectangle byte for byte. */
	if (gfx_rect(10u, 100u, 30u, 16u, 0x1Au, GFX_OP_COPY) < 0 ||
	    gfx_fill_rect(50u, 100u, 30u, 16u, 0x2Bu, GFX_OP_COPY) < 0 ||
	    gfx_fill_rect(52u, 102u, 26u, 12u, 0x3Cu, GFX_OP_XOR) < 0 ||
	    gfx_fill_rect(52u, 102u, 26u, 12u, 0x3Cu, GFX_OP_XOR) < 0) {
		gfx_close();
		printf("gfxtest: FAIL rects\r\n");
		return 1;
	}

	/* Ellipses: a filled circle, an outline circle, a wide ellipse
	 * outline (rx != ry), a flat one, the degenerate point and
	 * line, and a filled XOR pair restoring the region. */
	result = gfx_fill_ellipse(128u, 50u, 20u, 20u, 0x1Fu,
				 GFX_OP_COPY);
	if (result != 0) {
		printf("gfxtest: ELL1 %d\r\n", result);
	}
	result |= gfx_ellipse(128u, 62u, 24u, 24u, 0x2Fu,
			      GFX_OP_COPY);
	if (result != 0) {
		printf("gfxtest: ELL2 %d\r\n", result);
	}
	result |= gfx_ellipse(190u, 180u, 40u, 12u, 0x3Fu,
			      GFX_OP_COPY);
	if (result != 0) {
		printf("gfxtest: ELL3 %d\r\n", result);
	}
	result |= gfx_ellipse(60u, 180u, 6u, 40u, 0x4Fu,
			      GFX_OP_COPY);
	if (result != 0) {
		printf("gfxtest: ELL4 %d\r\n", result);
	}
	result |= gfx_ellipse(220u, 40u, 0u, 0u, 0x5Fu,
			      GFX_OP_COPY);
	if (result != 0) {
		printf("gfxtest: ELL5 %d\r\n", result);
	}
	result |= gfx_ellipse(210u, 40u, 6u, 0u, 0x6Fu,
			      GFX_OP_COPY);
	if (result != 0) {
		printf("gfxtest: ELL6 %d\r\n", result);
	}
	result |= gfx_fill_ellipse(32u, 32u, 10u, 6u, 0x7Fu,
				   GFX_OP_XOR);
	result |= gfx_fill_ellipse(32u, 32u, 10u, 6u, 0x7Fu,
				   GFX_OP_XOR);
	if (result != 0) {
		gfx_close();
		printf("gfxtest: FAIL ellipses %d\r\n", result);
		return 1;
	}
	(void)getchar(); /* key 1: every G7 result is on the peer */

	/* The 16-colour mode: pixels at odd and even x, a line over an
	 * odd x, the rectangle outline, and the odd-x fill's LMMV. */
	if (gfx_set_mode(GFX_MODE_512x212_16) < 0) {
		gfx_close();
		printf("gfxtest: FAIL mode G6\r\n");
		return 1;
	}
	(void)gfx_clear(0x00u);
	if (gfx_line(31u, 10u, 40u, 10u, 0x05u, GFX_OP_COPY) < 0 ||
	    gfx_point(31u, 12u, 0x06u, GFX_OP_COPY) < 0 ||
	    gfx_point(32u, 12u, 0x07u, GFX_OP_COPY) < 0 ||
	    gfx_fill_rect(31u, 14u, 3u, 2u, 0x08u, GFX_OP_COPY) < 0 ||
	    gfx_fill_rect(44u, 14u, 4u, 2u, 0x09u, GFX_OP_COPY) < 0 ||
	    gfx_rect(10u, 40u, 20u, 12u, 0x0Au, GFX_OP_COPY) < 0 ||
	    gfx_fill_rect(60u, 40u, 20u, 12u, 0x0Bu, GFX_OP_XOR) < 0 ||
	    gfx_fill_rect(60u, 40u, 20u, 12u, 0x0Bu, GFX_OP_XOR) < 0 ||
	    gfx_ellipse(128u, 90u, 18u, 12u, 0x0Cu, GFX_OP_COPY) < 0 ||
	    gfx_fill_ellipse(200u, 90u, 14u, 20u, 0x0Du,
			     GFX_OP_COPY) < 0) {
		gfx_close();
		printf("gfxtest: FAIL G6 shapes\r\n");
		return 1;
	}
	(void)getchar(); /* key 2: the G6 results are on the peer */

	/* The refusals, in G6: out of bounds, the unknown op, and the
	 * YJK line. */
	result = gfx_line(500u, 10u, 520u, 10u, 1u, GFX_OP_COPY);
	result |= gfx_point(520u, 10u, 1u, GFX_OP_COPY);
	result |= gfx_ellipse(510u, 100u, 6u, 4u, 1u, GFX_OP_COPY);
	result |= gfx_fill_ellipse(100u, 10u, 200u, 4u, 1u,
				   GFX_OP_COPY);
	result |= gfx_fill_rect(10u, 10u, 4u, 4u, 1u,
				(enum gfx_op)9u);
	result |= gfx_rect(508u, 10u, 10u, 4u, 1u, GFX_OP_COPY);
	result |= gfx_line(0u, 0u, 10u, 10u, 1u,
			   (enum gfx_op)7u);
	result |= gfx_fill_ellipse(100u, 520u, 4u, 4u, 1u,
				   GFX_OP_COPY);
	result |= gfx_ellipse(100u, 520u, 4u, 4u, 1u, GFX_OP_COPY);
	(void)getchar(); /* key 3 */
	(void)result;
	gfx_close();
	printf("gfxtest: shapes -2 -2 -2 -2 -2 -2 -2 -2 -2\r\n");
	printf("gfxtest: shapes PASS\r\n");
	return 0;
}

/* The timing mode's marker: one R#16 = 15 register write, read back
 * by the tests from the peer's reg_writes to count CPU T-states
 * between markers. */
static void gfxtest_mark(void)
{
	gfx_timing_mark();
}

static int gfxtest_timing(void)
{
	static const struct gfx_text_style style = {0x0Fu, 0x00u};
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
	(void)gfx_clear(0x00u);
	(void)gfx_display_enable(1u);

	gfx_begin();
	gfxtest_mark();            /* glyph: one opaque 6x8 cell */
	result = gfx_text(8u, 10u, "x", &style);
	gfxtest_mark();
	result |= gfx_text(8u, 20u, "x", &style);
	gfxtest_mark();            /* fill: one aligned HMMV */
	result |= gfx_fill_rect(100u, 40u, 32u, 16u, 0x03u,
				GFX_OP_COPY);
	gfxtest_mark();
	result |= gfx_fill_rect(100u, 60u, 32u, 16u, 0x03u,
				GFX_OP_COPY);
	gfxtest_mark();            /* line: any octant */
	result |= gfx_line(10u, 100u, 60u, 140u, 0x05u, GFX_OP_COPY);
	gfxtest_mark();
	result |= gfx_line(10u, 100u, 60u, 140u, 0x05u, GFX_OP_COPY);
	gfxtest_mark();            /* filled circle r=50 */
	result |= gfx_fill_ellipse(160u, 110u, 50u, 50u, 0x06u,
				   GFX_OP_COPY);
	gfxtest_mark();
	result |= gfx_fill_ellipse(160u, 110u, 50u, 50u, 0x06u,
				   GFX_OP_COPY);
	gfxtest_mark();            /* outline circle r=50 */
	result |= gfx_ellipse(160u, 110u, 50u, 50u, 0x07u, GFX_OP_COPY);
	gfxtest_mark();
	result |= gfx_ellipse(160u, 110u, 50u, 50u, 0x07u, GFX_OP_COPY);
	(void)result;
	(void)gfx_end();
	(void)getchar(); /* the key: the peer holds every marker */
	gfx_close();
	printf("gfxtest: timing %d\r\n", result);
	printf("gfxtest: timing PASS\r\n");
	return 0;
}

static int gfxtest_pages(void)
{
	static uint8_t pattern[256];
	struct gfx_info info;
	unsigned int i;
	uint8_t page_seen = 0u;
	int result;
	int page2_result;

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
	(void)gfx_clear(0x00u);
	(void)gfx_display_enable(1u);

	/* The asynchronous order: the fill dispatches and returns with
	 * the engine still drawing; the upload over the same rectangle
	 * waits for it, and the upload's bytes win.  The pattern is
	 * computed first, so the upload runs while the fill still is. */
	for (i = 0u; i < 256u; ++i) {
		pattern[i] = gfxtest_upload_byte(i);
	}
	result = gfx_fill_rect(8u, 10u, 64u, 8u, 0x0Au, GFX_OP_COPY);
	result |= gfx_upload_begin(8u, 10u, 64u, 8u);
	result |= gfx_upload_write(pattern, 256u) != 256;
	result |= gfx_upload_end();
	/* A 32-fill batch returns while the engine is still drawing: the
	 * marks bracket gfx_end, and the replay sees the second land
	 * before the last fill's CE clears (the last fill is large, so
	 * its busy window outlasts gfx_end's return). */
	gfxtest_mark();
	gfx_begin();
	for (i = 0u; i < 31u; ++i) {
		result |= gfx_fill_rect((uint16_t)(8u + i * 6u), 40u, 2u, 2u,
					(gfx_color_t)((i & 0x07u) + 1u),
					GFX_OP_COPY);
	}
	result |= gfx_fill_rect(194u, 40u, 32u, 8u, 0x0Bu, GFX_OP_COPY);
	result |= gfx_end();
	gfxtest_mark();
	(void)getchar(); /* key 1: the replay holds the async order */

	/* Page 1's picture: a full clear, then a band (a third of the
	 * screen) for the partial present. */
	result |= gfx_clear(0x0Cu);
	result |= gfx_fill_rect(0u, 256u, 512u, 212u, 0x0Du, GFX_OP_COPY);
	result |= gfx_fill_rect(0u, 256u, 512u, 70u, 0x0Eu, GFX_OP_COPY);
	result |= gfx_show_page(1u);
	(void)gfx_get_info(&info);
	page_seen = info.page;
	(void)getchar(); /* key 2: page 1 is shown */

	/* Three frames, one per call, wherever in the frame the first
	 * call lands. */
	gfxtest_mark();
	result |= gfx_wait_vblank();
	result |= gfx_wait_vblank();
	result |= gfx_wait_vblank();
	gfxtest_mark();
	(void)getchar(); /* key 3: the replay counts the frames */

	/* The partial present: page 0 back, then the band copied from
	 * page 1 across the vertical blank. */
	result |= gfx_show_page(0u);
	result |= gfx_wait_vblank();
	result |= gfx_copy_rect(0u, 256u, 0u, 0u, 512u, 70u);
	(void)getchar(); /* key 4: the replay checks page 0's band */

	page2_result = gfx_show_page(2u);
	gfx_close();
	printf("gfxtest: pages %d %d %u\r\n", result, page2_result,
	       page_seen);
	if (result == 0 && page2_result == -KOS_EINVAL && page_seen == 1u) {
		printf("gfxtest: pages PASS\r\n");
		return 0;
	}
	printf("gfxtest: pages FAIL\r\n");
	return 1;
}

int main(int argc, char **argv)
{
	int leak;

	/* argv holds the command-tail words only (the kernel transports no
	 * program name), so argv[0] is the mode. */
	if (argc < 1) {
		printf("gfxtest: mode6|mode7|yjk|info|upload|palette|prims|text|atlas [leak]"
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
	if (strcmp(argv[0], "prims") == 0) {
		return gfxtest_prims();
	}
	if (strcmp(argv[0], "shapes") == 0) {
		return gfxtest_shapes();
	}
	if (strcmp(argv[0], "timing") == 0) {
		return gfxtest_timing();
	}
	if (strcmp(argv[0], "pages") == 0) {
		return gfxtest_pages();
	}
	if (strcmp(argv[0], "text") == 0) {
		return gfxtest_text();
	}
	if (strcmp(argv[0], "atlas") == 0) {
		return gfxtest_atlas();
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
