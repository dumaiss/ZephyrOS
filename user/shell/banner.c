/* KonaOS native shell: the startup banner.
 *
 * One part of the shell like the others, drawn once before the read-run
 * loop: the text lines the canvas mock-up places on the GRAPHIC 6
 * console (85 columns of 6 pixels, 26 rows of 8 lines) and, when the
 * build publishes a VIDEO driver, the logo - the GRAPHIC 6 bitmap in
 * logo.c, written straight into VRAM over the public VIDEO protocol, one
 * row at a time: a CTRL_WRITE sets R#14 and the row's VRAM address, a
 * DATA_WRITE streams its 40 bytes through the data port, which the
 * hardware WAIT paces.  The command engine is not used, so nothing waits
 * on its transfer-ready flag.  A serial build prints the same text; the
 * logo bind simply fails where no video driver is published.  The logo
 * is screen decoration only: a later clear overwrites it like any text.
 *
 * The console reset the launcher runs before every start leaves the
 * terminal home on a clean screen, so the banner always paints on the
 * same canvas.
 */

#include "kos.h"
#include "kos/protocol.h"
#include "diag.h" /* the SDK diag wrappers */

#include "shell.h"
#include "logo.h"

/* The logo's top-left corner in the page-zero bitmap (the canvas
 * mock-up's placement): x in pixels (even: two pixels per byte), y in
 * lines from the terminal's row 0, which the console reset puts at
 * bitmap line 0. */
#define BANNER_LOGO_X       8u
#define BANNER_LOGO_Y       10u

/* Cursor addresses (ANSI CUP, 1-based).  The text columns line up with
 * the logo: 16 cells = 96 pixels clears the 87-pixel logo width. */
#define BANNER_TITLE_POS     "\x1b[3;17H"
#define BANNER_VERSION_POS   "\x1b[5;17H"
#define BANNER_IOC_POS       "\x1b[6;17H"
#define BANNER_COPYRIGHT_POS "\x1b[8;17H"
#define BANNER_PROMPT_POS    "\x1b[11;1H"

/* The IO controller's PING (the controller's own vocabulary, which the
 * IOC protocol passes through): CMD 01h, reply 81h, and the firmware level
 * at reply DATA offset 16, after the 16 echoed request bytes.  The IOC
 * reply frame is TYPE, STATUS, LENGTH, DATA..., so DATA[16] is frame
 * byte 19. */
#define BANNER_IOC_PING       0x01u
#define BANNER_IOC_PING_REPLY 0x81u
#define BANNER_IOC_LEVEL_DATA 16u
#define BANNER_IOC_FRAME_DATA 3u

/* The command-port register select for R#14 (VRAM A16..A14), and the
 * address byte's write flag. */
#define BANNER_REG_R14_SELECT (0x0Eu | 0x80u)
#define BANNER_VRAM_WRITE     0x40u

static const char banner_months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";

/* `__DATE__` ("Oct 20 2026") as the ISO "YYYY-MM-DD" of the canvas. */
static void shell_build_date(char out[11])
{
	const char *date = __DATE__;
	unsigned int month = 0u;
	unsigned int tens;
	unsigned int index;

	for (index = 0u; index < 12u; ++index) {
		if (date[0] == banner_months[index * 3u] &&
		    date[1] == banner_months[index * 3u + 1u] &&
		    date[2] == banner_months[index * 3u + 2u]) {
			month = index + 1u;
			break;
		}
	}
	tens = month >= 10u ? 1u : 0u;
	out[0] = date[7];
	out[1] = date[8];
	out[2] = date[9];
	out[3] = date[10];
	out[4] = '-';
	out[5] = (char)('0' + tens);
	out[6] = (char)('0' + (month - 10u * tens));
	out[7] = '-';
	out[8] = date[4] == ' ' ? '0' : date[4];
	out[9] = date[5];
	out[10] = '\0';
}

/* One native VIDEO operation through a bound handle. */
static int banner_video_send(int handle, unsigned char op,
			     const unsigned char *payload,
			     unsigned int length)
{
	struct kos_ipc_request req;

	req.frame.op = KVIDEO_OP_SEND;
	req.frame.flags = 0u;
	req.frame.desc = KOS_IPC_DESC_NONE;
	req.frame.arg0 = op;
	req.frame.arg1 = length;
	req.buffer = (void *)payload;
	req.length = length;
	req.direction = KOS_IPC_DESC_IN;
	return call((unsigned char)handle, &req);
}

/* Put the logo on the screen, one VRAM row at a time.  A machine without
 * a video driver, or a video handle that refuses, keeps its text-only
 * banner. */
static void shell_logo_blit(void)
{
	unsigned char type = 0u;
	int handle = bind("/dev/video", 0u, &type);
	unsigned char address[4];
	unsigned int row;

	if (handle < 0) {
		return;
	}
	if (type != KOS_TYPE_VIDEO) {
		(void)unbind((unsigned char)handle);
		return;
	}
	/* The terminal's last glyph copies must finish before the VRAM
	 * writes (PRESENT waits for the command engine). */
	if (banner_video_send(handle, KVIDEO_PRESENT, 0u, 0u) < 0) {
		(void)unbind((unsigned char)handle);
		return;
	}
	for (row = 0u; row < KONAOS_LOGO_HEIGHT; ++row) {
		/* G6: 256 bytes per line, two pixels per byte. */
		unsigned int vram = (BANNER_LOGO_Y + row) * 256u +
				    BANNER_LOGO_X / 2u;

		address[0] = (unsigned char)((vram >> 14) & 0x03u); /* R#14 */
		address[1] = BANNER_REG_R14_SELECT;
		address[2] = (unsigned char)(vram & 0xFFu);
		address[3] = (unsigned char)(((vram >> 8) & 0x3Fu) |
					     BANNER_VRAM_WRITE);
		if (banner_video_send(handle, KVIDEO_CTRL_WRITE,
				      address, 4u) < 0 ||
		    banner_video_send(handle, KVIDEO_DATA_WRITE,
				      &konaos_logo[row * KONAOS_LOGO_ROW_BYTES],
				      KONAOS_LOGO_ROW_BYTES) < 0) {
			break;
		}
	}
	(void)unbind((unsigned char)handle);
}

/* The native ABI level the running kernel reports (its system-info
 * record), or -1 when it does not answer. */
static int shell_abi_level(void)
{
	struct kos_diag_system_info info;

	if (diag_system_info(&info) != KOS_DIAG_SYSTEM_INFO_SIZE) {
		return -1;
	}
	return (int)info.abi;
}

/* The IO controller's firmware level from one PING, or -1 when the
 * build publishes no IOC driver or the controller does not answer. */
static int shell_ioc_level(void)
{
	struct kos_ipc_request req;
	unsigned char type = 0u;
	unsigned char reply[KIOC_PROTOCOL_FRAME_SIZE];
	int handle = bind("/dev/ioc", 0u, &type);
	int result = -1;

	if (handle < 0) {
		return -1;
	}
	if (type == KOS_TYPE_IOC) {
		req.frame.op = KIOC_OP_COMMAND;
		req.frame.flags = 0u;
		req.frame.desc = KOS_IPC_DESC_NONE;
		req.frame.arg0 = BANNER_IOC_PING;
		req.frame.arg1 = 0u;          /* no echo pattern       */
		req.buffer = (void *)0;
		req.length = 0u;
		req.direction = KOS_IPC_DESC_IN;
		if (call((unsigned char)handle, &req) >= 0 &&
		    req.frame.arg0 == BANNER_IOC_PING_REPLY &&
		    req.frame.payload[2] == 0u &&
		    req.frame.arg1 > BANNER_IOC_LEVEL_DATA) {
			/* The copy lands at the same offset in the caller's
			 * buffer as in the stored frame: take the whole
			 * frame. */
			req.frame.op = KIOC_OP_REPLY;
			req.frame.desc = KOS_IPC_DESC_NONE;
			req.frame.arg0 = 0u;
			req.frame.arg1 = KIOC_PROTOCOL_FRAME_SIZE;
			req.buffer = reply;
			req.length = KIOC_PROTOCOL_FRAME_SIZE;
			req.direction = KOS_IPC_DESC_OUT;
			if (call((unsigned char)handle, &req) ==
			    (int)KIOC_PROTOCOL_FRAME_SIZE) {
				result = (int)reply[BANNER_IOC_FRAME_DATA +
						    BANNER_IOC_LEVEL_DATA];
			}
		}
	}
	(void)unbind((unsigned char)handle);
	return result;
}

/* "<label> <level>", or "<label> unknown". */
static void shell_banner_level(const char *label, int level)
{
	console_puts(label);
	if (level < 0) {
		console_puts("unknown");
	} else {
		printf("%d", level);
	}
}

void shell_banner(void)
{
	char date[11];

	shell_build_date(date);
	console_puts(BANNER_TITLE_POS "KonaOS v1.0 (build date: ");
	console_puts(date);
	console_puts(")");
	shell_banner_level(BANNER_VERSION_POS "ABI version ", shell_abi_level());
	shell_banner_level(BANNER_IOC_POS "IOC version ", shell_ioc_level());
	console_puts(BANNER_COPYRIGHT_POS
		     "Copyright (C) Sebastien Dumais 2026");
	console_puts(BANNER_PROMPT_POS);
	/* The text lines reach the driver before the logo's VRAM writes. */
	(void)console_flush();
	shell_logo_blit();
}
