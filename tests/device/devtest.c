/* KonaOS device-release outcome program (test artifact).
 *
 * One program, one mode per tail word; tests/test_device_release.py
 * starts it from the monitor or the shell and asserts the transcript,
 * the V9958 peer trace and the card peer's state.  Every mode reports on
 * two sinks: the console (the V9958, which a takeover stops drawing) and
 * the serial lane, so a watcher still sees progress the video path is
 * deliberately dropping.
 *
 *   claim    bind `/dev/video` exclusive, reprogram the VDP for GRAPHIC 4
 *            (a mode the console never selects) and scribble both the
 *            visible bitmap and the font atlas, then exit *while
 *            holding*: the exit release must restore the display;
 *   release  the same takeover, released explicitly with unbind, then one
 *            console line that must be drawn again;
 *   busy     the exclusive-handle rules: a second exclusive bind is
 *            refused with -KOS_EBUSY, release then acquire works, and a
 *            device whose driver declares no release answers
 *            -KOS_ENOTSUP;
 *   poke     write VDP registers and VRAM with no handle at all, then
 *            exit: the launcher's console reset must restore the display
 *            before the next start of the boot program;
 *   write    open a card file, write one chunk (leaving the controller
 *            slot open) and warm-reboot in the middle of the transfer.
 *
 * Each held-mode console copy is one WRITE call of a known length, so
 * the driver's drop counter must grow by exactly that length and no
 * glyph of the text may be drawn.
 */

#include "kos.h"
#include "diag.h" /* the SDK diag wrappers (not in kos.h) */
#include "kos/protocol.h"

#define LANE_NAME    "/dev/serial/0"
#define VIDEO_NAME   "/dev/video"
#define CONSOLE_NAME "/dev/console"
#define CARD_FILE    "/fs/DEVTEST.DAT"

/* V9958 ports and the registers the takeover touches.  R#0 bit 2 is M4,
 * so 04h selects GRAPHIC 4. */
#define VDP_PORT_DATA    0xA0u
#define VDP_PORT_CTRL    0xA1u
#define VDP_PORT_PALETTE 0xA2u
#define VDP_REG_R0       0x00u
#define VDP_REG_R7       0x07u
#define VDP_REG_R14      0x0Eu
#define VDP_REG_R16      0x10u
#define VDP_MODE_GRAPHIC4 0x04u
#define VDP_BORDER_TAKEN 0x0Eu

/* The scribbles: the visible bitmap (page 0, address 0) and the font
 * atlas (page 1, address 0), both rewritten by the display reset. */
#define VISIBLE_ADDR  0x0000u
#define ATLAS_ADDR    0x0000u
#define SCRIBBLE_LEN  16u
#define SCRIBBLE_BYTE 0x5Au

/* Lines the tests pin.  The two console-only copies are dropped while a
 * takeover holds the device and are the lengths the drop counter grows
 * by. */
#define CLAIM_LINE    "dev: claim ok\r\n"
#define HIDDEN_LINE   "dev: hidden\r\n"
#define SHORT_LINE    "dev: \r\n"
#define DRAWN_LINE    "dev: drawn\r\n"
#define HOLD_LINE     "dev: hold\r\n"
#define LEAKED_LINE   "dev: leaked\r\n"
#define POKED_LINE    "dev: poked\r\n"
#define WRITING_LINE  "dev: writing\r\n"
#define BUSY_END_LINE "dev: busy end\r\n"

/* 66-byte payload the mid-transfer reboot leaves half-written. */
static const char dev_payload[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
	"abcdefghijklmnopqrstuvwxyz,.!?";

static char dev_line[80];
static unsigned char dev_lane;

/* --- output ----------------------------------------------------------- */

static void dev_lane_write(const char *text, unsigned int length)
{
	struct kos_ipc_request req;

	if (dev_lane == 0u) {
		return;
	}
	req.frame.op = KSIO_OP_WRITE;
	req.frame.flags = 0u;
	req.frame.arg0 = 0u;
	req.frame.arg1 = length;
	req.buffer = (void *)text;
	req.length = length;
	req.direction = KOS_IPC_DESC_IN;
	if (call(dev_lane, &req) < 0) {
		dev_lane = 0u;
	}
}

/* One line to the console and, so a dropped console copy cannot hide
 * it, to the lane.  Returns the console's own result: a takeover's drop
 * still reports the whole length to the caller. */
static int dev_report(const char *text, unsigned int length)
{
	int result = console_write(text, length);

	dev_lane_write(text, length);
	return result;
}

static int dev_send(const char *text)
{
	return dev_report(text, (unsigned int)strlen(text));
}

static void dev_lane_open(void)
{
	unsigned char type = 0u;
	int handle = bind(LANE_NAME, 0u, &type);

	if (handle >= (int)KOS_HANDLE_FIRST_BOUND && type == KOS_TYPE_SERIAL) {
		dev_lane = (unsigned char)handle;
	}
}

/* --- V9958 access through the diagnostic boundary --------------------- */

static void vdp_out(unsigned char port, unsigned char value)
{
	struct kos_diag_io_out_args req;

	req.port = port;
	req.value = value;
	(void)diag_io_out(&req);
}

static void vdp_reg(unsigned char reg, unsigned char value)
{
	vdp_out(VDP_PORT_CTRL, value);
	vdp_out(VDP_PORT_CTRL, (unsigned char)(0x80u | reg));
}

/* Select a VRAM address below the given 16-KiB page: A16 goes to R#14
 * (bits A16:A14), then the two address words follow the register write. */
static void vdp_addr(unsigned char a16, unsigned int addr)
{
	vdp_reg(VDP_REG_R14, (unsigned char)((unsigned char)(a16 << 2) |
					     (unsigned char)((addr >> 14) & 0x03u)));
	vdp_out(VDP_PORT_CTRL, (unsigned char)(addr & 0xFFu));
	vdp_out(VDP_PORT_CTRL, (unsigned char)(0x40u | ((addr >> 8) & 0x3Fu)));
}

static void vdp_scribble(unsigned char a16, unsigned int addr)
{
	unsigned char i;

	vdp_addr(a16, addr);
	for (i = 0u; i < SCRIBBLE_LEN; ++i) {
		vdp_out(VDP_PORT_DATA, SCRIBBLE_BYTE);
	}
}

/* The takeover: another display mode, border and palette, and scribbles
 * over both VRAM pages the display reset must rewrite. */
static void vdp_takeover(void)
{
	unsigned char i;

	vdp_reg(VDP_REG_R0, VDP_MODE_GRAPHIC4);
	vdp_reg(VDP_REG_R7, VDP_BORDER_TAKEN);
	vdp_reg(VDP_REG_R16, 0u);
	for (i = 0u; i < 32u; ++i) {
		vdp_out(VDP_PORT_PALETTE, 0x77u);
	}
	vdp_scribble(0u, VISIBLE_ADDR);
	vdp_scribble(1u, ATLAS_ADDR);
}

/* --- modes ------------------------------------------------------------ */

/* The takeover every held mode runs: bind exclusive, report, take the
 * display over, drop two console copies of different lengths, report the
 * end of the takeover, and then either release the handle or exit while
 * still holding it. */
static int dev_hold(unsigned char explicit_release)
{
	unsigned char type = 0u;
	int results[5];
	int handle = bind(VIDEO_NAME, KOS_BIND_EXCLUSIVE, &type);

	if (handle < (int)KOS_HANDLE_FIRST_BOUND) {
		(void)snprintf(dev_line, sizeof(dev_line),
			       "dev: claim %d\r\n", handle);
		dev_send(dev_line);
		return 2;
	}
	results[0] = dev_send(CLAIM_LINE);
	vdp_takeover();
	results[1] = dev_send(HIDDEN_LINE);
	results[2] = dev_send(HOLD_LINE);
	results[3] = dev_send(SHORT_LINE);
	results[4] = dev_send(LEAKED_LINE);
	(void)snprintf(dev_line, sizeof(dev_line),
		       "dev: took %d %d %d %d %d\r\n", results[0], results[1],
		       results[2], results[3], results[4]);
	dev_lane_write(dev_line, (unsigned int)strlen(dev_line));
	if (explicit_release != 0u) {
		if (unbind((unsigned char)handle) != 0) {
			dev_send("dev: unbind\r\n");
			return 3;
		}
		dev_send(DRAWN_LINE); /* drawn again: the reset ran */
	}
	return 0;
}

static int dev_busy(void)
{
	unsigned char type = 0u;
	int first;
	int second;
	int third;
	int console;
	int lane;
	int status = 0;

	first = bind(VIDEO_NAME, KOS_BIND_EXCLUSIVE, &type);
	second = bind(VIDEO_NAME, KOS_BIND_EXCLUSIVE, &type);
	if (first < (int)KOS_HANDLE_FIRST_BOUND) {
		(void)snprintf(dev_line, sizeof(dev_line),
			       "dev: first %d\r\n", first);
		dev_send(dev_line);
		return 2;
	}
	dev_send("dev: first ok\r\n");
	(void)snprintf(dev_line, sizeof(dev_line), "dev: second %d\r\n",
		       second);
	dev_send(dev_line);
	if (unbind((unsigned char)first) != 0) {
		dev_send("dev: unbind\r\n");
		return 3;
	}
	third = bind(VIDEO_NAME, KOS_BIND_EXCLUSIVE, &type);
	if (third < (int)KOS_HANDLE_FIRST_BOUND) {
		(void)snprintf(dev_line, sizeof(dev_line),
			       "dev: third %d\r\n", third);
		dev_send(dev_line);
		return 4;
	}
	dev_send("dev: third ok\r\n");
	console = bind(CONSOLE_NAME, KOS_BIND_EXCLUSIVE, &type);
	(void)snprintf(dev_line, sizeof(dev_line), "dev: console %d\r\n",
		       console);
	dev_send(dev_line);
	lane = bind(LANE_NAME, KOS_BIND_EXCLUSIVE, &type);
	(void)snprintf(dev_line, sizeof(dev_line), "dev: lane %d\r\n", lane);
	dev_send(dev_line);
	if (unbind((unsigned char)third) != 0) {
		dev_send("dev: unbind\r\n");
		return 5;
	}
	dev_send(BUSY_END_LINE);
	return status;
}

static int dev_poke(void)
{
	vdp_takeover();
	dev_send(POKED_LINE);
	return 0;
}

/* The console reset itself: poke the display with no handle, then drive
 * KCONSOLE_OP_RESET on the console object and report what it answered. */
static int dev_creset(void)
{
	struct kos_ipc_request req;
	unsigned char type = 0u;
	int handle = bind(CONSOLE_NAME, 0u, &type);
	int result;

	if (handle < (int)KOS_HANDLE_FIRST_BOUND) {
		(void)snprintf(dev_line, sizeof(dev_line),
			       "dev: console bind %d\r\n", handle);
		dev_send(dev_line);
		return 2;
	}
	vdp_takeover();
	dev_send(POKED_LINE);
	req.frame.op = KCONSOLE_OP_RESET;
	req.frame.flags = 0u;
	req.frame.desc = KOS_IPC_DESC_NONE;
	req.frame.arg0 = 0u;
	req.frame.arg1 = 0u;
	req.buffer = (void *)0;
	req.length = 0u;
	req.direction = KOS_IPC_DESC_NONE;
	result = call((unsigned char)handle, &req);
	(void)snprintf(dev_line, sizeof(dev_line), "dev: creset %d\r\n",
		       result);
	dev_send(dev_line);
	return 0;
}

/* The controller slot stays open across the reboot: the file is written
 * through one open instance and the driver is never told to close it. */
static int dev_write(void)
{
	int fd = open(CARD_FILE, KFS_O_WRITE | KFS_O_CREATE | KFS_O_TRUNC);
	int written;

	if (fd < 0) {
		(void)snprintf(dev_line, sizeof(dev_line), "dev: card %d\r\n",
			       fd);
		dev_send(dev_line);
		return 2;
	}
	written = write((unsigned int)fd, dev_payload,
			(unsigned int)(sizeof(dev_payload) - 1u));
	(void)snprintf(dev_line, sizeof(dev_line), "dev: writing %d\r\n",
		       written);
	dev_send(dev_line);
	reboot();
	return 0;
}

static int dev_run(const char *mode)
{
	if (strcmp(mode, "claim") == 0) {
		return dev_hold(0u);
	}
	if (strcmp(mode, "release") == 0) {
		return dev_hold(1u);
	}
	if (strcmp(mode, "busy") == 0) {
		return dev_busy();
	}
	if (strcmp(mode, "poke") == 0) {
		return dev_poke();
	}
	if (strcmp(mode, "creset") == 0) {
		return dev_creset();
	}
	if (strcmp(mode, "write") == 0) {
		return dev_write();
	}
	dev_send("dev: mode\r\n");
	return 1;
}

int main(int argc, char **argv)
{
	dev_lane_open();
	/* argv holds the command-tail words only, so the mode is argv[0]. */
	if (argc < 1) {
		dev_send("dev: mode\r\n");
		return 1;
	}
	return dev_run(argv[0]);
}
