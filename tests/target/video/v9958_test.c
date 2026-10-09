/* ZephyrOS V9958 video service acceptance selftest (test artifact,
 * work package 41).
 *
 * The kernel calls zos_run_video_selftest() from the late boot hook,
 * after the resident service loader has placed and started this image's
 * service set (`sio v9958 console`, the loader's `service ...` evidence
 * lines precede this transcript) and after the nucleus console probe
 * wrote `console: service OK` through the console service - the console
 * composes the V9958 output with the serial input fallback this store
 * implies, so that line is visible in the V9958 command trace.
 *
 * This diagnostic is a *bank-7 client* of the video service (see
 * tests/target/include/service_client.h): it holds no descriptor and
 * cannot use `move`, so it drives the descriptor-less protocol forms
 * only, and every control/outcome line goes through the direct early SIO
 * so the transcript is independent of the console service it exercises.
 *
 * Coverage (abi/zos/protocol.h section ZOS_CLASS_VIDEO):
 *
 *   - endpoint discovery: the VIDEO endpoint answers at
 *     ZOS_PROTOCOL_VERSION (class 02, version 01), and the serial and
 *     console endpoints are present (the console is this service's
 *     composed client; the serial service is the early-console and
 *     console-input fallback);
 *   - ZVIDEO_PRESENT (argument-free) answers 0: the registered state is
 *     live and the command engine drained;
 *   - ZVIDEO_RESET (argument-free) answers 0 and re-runs the complete
 *     display bring-up: the replay observes the re-programmed GRAPHIC 6
 *     register sequence and the font atlas upload fed by this image's
 *     bulk segment;
 *   - ZVIDEO_OP_PUTC renders the marker `V41` through the terminal
 *     renderer after the reset (which clears the text plane); the
 *     replay decodes the glyph copies from the V9958 command trace;
 *   - malformed calls are refused with -ZOS_EINVAL and cause no VDP
 *     access: an unknown protocol op, a streaming native op without a
 *     caller buffer (a bank-7 client can never hold the descriptor the
 *     five streaming operations require), and a handle the nucleus
 *     never issued;
 *   - the transcript's last line is the pinned `video selftest: OK`
 *     (or `video selftest: FAIL <phase>`), which tests/test_v9958_sim.py
 *     pins along with the peer-observable facts above.
 */

#include "zos/protocol.h"

#include "zephyros/errno.h"
#include "zephyros/video.h"

#include "service_client.h"
#include "video_selftest.h"

#if ZOS_VIDEO_SELFTEST || ZOS_VIDEO_FONTFAIL

uint8_t ace_video_selftest_result;

#define VIDEO_TAG "video: "
/* The terminal marker the replay decodes from the V9958 glyph trace. */
#define VIDEO_MARKER "V41"

static const char video_marker[] = VIDEO_MARKER "\r\n";
static uint8_t video_handle;

static void video_puts(const char *text)
{
	service_cli_puts(VIDEO_TAG);
	service_cli_puts(text);
	service_cli_puts("\r\n");
}

/* One failure line, and the diagnostic's last line by construction. */
static void video_fail(uint8_t code, const char *phase)
{
	ace_video_selftest_result = code;
	service_cli_puts("video selftest: FAIL ");
	service_cli_puts(phase);
	service_cli_puts("\r\n");
}

/* Endpoint discovery row: the video class and the running protocol
 * version, then the two peer endpoints this store implies. */
static uint8_t video_probe_endpoints(void)
{
	uint8_t handle = 0u;
	uint8_t serial;
	uint8_t console;

	service_cli_puts(VIDEO_TAG "endpoint class ");
	service_cli_hex8(ZOS_CLASS_VIDEO);
	service_cli_puts(" version ");
	service_cli_hex8(ZOS_PROTOCOL_VERSION);
	service_cli_puts("\r\n");

	serial = service_cli_endpoint(ZOS_CLASS_SERIAL, &handle);
	console = service_cli_endpoint(ZOS_CLASS_CONSOLE, &handle);
	service_cli_puts(VIDEO_TAG "peers serial=");
	service_cli_puts(serial != 0u ? "01" : "00");
	service_cli_puts(" console=");
	service_cli_puts(console != 0u ? "01" : "00");
	service_cli_puts("\r\n");
	return (uint8_t)(serial != 0u && console != 0u);
}

void zos_run_video_selftest(void)
{
	struct zos_ipc_frame frame;
	uint8_t index;

	ace_video_selftest_result = ZOS_VIDEOSELFTEST_FAIL_ENDPOINT;

	if (service_cli_endpoint(ZOS_CLASS_VIDEO, &video_handle) == 0u ||
	    video_probe_endpoints() == 0u) {
		video_fail(ZOS_VIDEOSELFTEST_FAIL_ENDPOINT, "endpoint");
		return;
	}

	/* Presence: PRESENT flushes the pending terminal output and waits
	 * for the command engine; a live service answers 0. */
	service_cli_frame(&frame, ZVIDEO_OP_SEND, ZVIDEO_PRESENT, 0u);
	if (service_cli_call(video_handle, &frame) != 0) {
		video_fail(ZOS_VIDEOSELFTEST_FAIL_PRESENT, "present");
		return;
	}
	video_puts("present OK");

	/* The display reset: re-runs the whole bring-up, including the font
	 * and atlas upload, which reads the image's bulk segment through
	 * the nucleus bulk copy at run time. */
	service_cli_frame(&frame, ZVIDEO_OP_SEND, ZVIDEO_RESET, 0u);
	if (service_cli_call(video_handle, &frame) != 0) {
		video_fail(ZOS_VIDEOSELFTEST_FAIL_RESET, "reset");
		return;
	}
	video_puts("reset OK");

	/* The terminal marker, drawn after the reset cleared the plane: the
	 * glyph copies are the replay's visible side of this diagnostic. */
	for (index = 0u; video_marker[index] != '\0'; ++index) {
		if (service_cli_putc(video_handle, ZVIDEO_OP_PUTC,
				     (uint8_t)video_marker[index]) != 0) {
			video_fail(ZOS_VIDEOSELFTEST_FAIL_PUTC, "putc");
			return;
		}
	}
	video_puts("marker " VIDEO_MARKER);

	/* Malformed calls: refused exactly, and no VDP access happens in
	 * this phase (the replay proves that from the frozen peer trace). */
	service_cli_frame(&frame, 0x40u, 0u, 0u);
	if (service_cli_call(video_handle, &frame) != -ZOS_EINVAL) {
		video_fail(ZOS_VIDEOSELFTEST_FAIL_MALFORMED, "unknown-op");
		return;
	}
	/* A streaming native op names a caller descriptor; a bank-7 client
	 * holds none, so the service refuses it before any port I/O. */
	service_cli_frame(&frame, ZVIDEO_OP_SEND, ZVIDEO_CTRL_WRITE, 2u);
	if (service_cli_call(video_handle, &frame) != -ZOS_EINVAL) {
		video_fail(ZOS_VIDEOSELFTEST_FAIL_MALFORMED, "no-descriptor");
		return;
	}
	/* A handle the nucleus never issued is refused by the substrate. */
	service_cli_frame(&frame, ZVIDEO_OP_PUTC, (uint8_t)'x', 0u);
	if (service_cli_call(0xFEu, &frame) != -ZOS_EINVAL) {
		video_fail(ZOS_VIDEOSELFTEST_FAIL_MALFORMED, "handle");
		return;
	}
	video_puts("malformed rejected");

	ace_video_selftest_result = ZOS_VIDEOSELFTEST_OK;
	service_cli_puts("video selftest: OK\r\n");
}

#endif /* ZOS_VIDEO_SELFTEST || ZOS_VIDEO_FONTFAIL */
