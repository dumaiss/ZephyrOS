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

int main(int argc, char **argv)
{
	int leak;

	/* argv holds the command-tail words only (the kernel transports no
	 * program name), so argv[0] is the mode. */
	if (argc < 1) {
		printf("gfxtest: mode6|mode7|yjk|info [leak]\r\n");
		return 1;
	}
	leak = argc > 1 && strcmp(argv[1], "leak") == 0;
	if (strcmp(argv[0], "info") == 0) {
		return gfxtest_info();
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
