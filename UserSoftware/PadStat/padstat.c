/* PadStat for KonaOS.
 *
 * Passive USB/HID enumeration status from the IOC.  Reads HID_STATUS page 0
 * (controller and keyboard) and page 6 (devices, last HID interface and the
 * two F310 slots) through the published /dev/ioc object.  The controller
 * answers both pages from snapshots kept by its foreground TinyUSB callbacks:
 * no USB transaction runs and no interrupt is acknowledged.
 *
 * /W watches instead: it polls both pages every 250 ms and prints one compact
 * line whenever anything changes, so plugging and unplugging a device leaves a
 * trail.  Any key ends it.
 */

#include "kos.h"
#include "kos/timer.h"

/* KonaOS /dev/ioc operations; framing, sequencing and recovery stay in the
 * resident IOC driver. */
#define KIOC_OP_COMMAND       0u
#define KIOC_OP_REPLY         1u
#define KIOC_FRAME_SIZE       29u

#define IOC_CMD_HID_STATUS    0x0Du
#define IOC_RSP_HID_STATUS    0x8Du
#define IOC_RSP_UNKNOWN       0xFEu
#define HID_PAGE_STATUS       0x00u
#define HID_PAGE_GAMEPAD      0x06u
#define HID_PAGE_OTHER        0x07u
#define HID_PAGE_LEN          26u
#define HID_OTHER_LEN         21u

/* Page 0 payload offsets. */
#define P0_STATUS             0u
#define P0_REVISION           1u
#define P0_DEVICES            13u
#define P0_KBD_ADDR           14u
#define P0_KBD_REPORTS        15u
#define P0_LAST_REPORT        17u
#define P0_KBD_SPEED          25u

/* Page 6 payload offsets. */
#define P6_PAGE               0u
#define P6_DEVICES            1u
#define P6_MOUNTS             2u
#define P6_UNMOUNTS           3u
#define P6_LAST_ADDR          4u
#define P6_LAST_VID           5u
#define P6_LAST_PID           7u
#define P6_HID_MOUNTS         9u
#define P6_LAST_HID_ADDR      10u
#define P6_PAD0               11u
#define P6_PAD1               18u
#define P6_LAST_HID_PROTO     25u

/* Within one pad slot: addr, instance, first arm, reports LE, len, latch. */
#define PAD_ADDR              0u
#define PAD_INST              1u
#define PAD_ARM               2u
#define PAD_REPORTS           3u
#define PAD_LEN               5u
#define PAD_LATCH             6u

/* Page 7 payload offsets: the HID interface that is neither the keyboard nor
 * an F310 (a dongle's mouse), raw. */
#define P7_PAGE               0u
#define P7_ADDR               1u
#define P7_INST               2u
#define P7_REPORTS            3u
#define P7_LEN                5u
#define P7_LAST               6u
#define P7_BUTTONS            14u
#define P7_X                  15u
#define P7_Y                  17u
#define P7_WHEEL              19u

#define F310_VID              0x046Du
#define F310_DIRECTINPUT_PID  0xC216u

#define WATCH_PERIOD_US       250000ul

static unsigned char ioc_handle;
static unsigned char page0[HID_PAGE_LEN];
static unsigned char page6[HID_PAGE_LEN];
static unsigned char last0[HID_PAGE_LEN];
static unsigned char last6[HID_PAGE_LEN];
static unsigned char page7[HID_OTHER_LEN];
static unsigned char last7[HID_OTHER_LEN];
static unsigned char have_page7;

static unsigned int le16(const unsigned char *data)
{
	return (unsigned int)data[0] | ((unsigned int)data[1] << 8);
}

/* Submit one controller command through the resident IOC driver, then copy
 * its stored native reply into this process through the descriptor form. */
static int ioc_command(unsigned char command, const unsigned char *data,
		       unsigned char length, unsigned char expected,
		       unsigned char *reply_data, unsigned char reply_length)
{
	struct kos_ipc_request request;
	unsigned char native[KIOC_FRAME_SIZE];
	int result;

	memset(&request, 0, sizeof(request));
	request.frame.op = KIOC_OP_COMMAND;
	request.frame.arg0 = command;
	request.frame.arg1 = length;
	memcpy(request.frame.payload, data, length);
	result = call(ioc_handle, &request);
	if (result < 0) {
		return result;
	}
	if (request.frame.arg0 == IOC_RSP_UNKNOWN) {
		return -KOS_ENOSYS;
	}
	if (request.frame.arg0 != expected ||
	    request.frame.arg1 != reply_length ||
	    request.frame.payload[2] != 0u) {
		return -KOS_EIO;
	}

	memset(&request, 0, sizeof(request));
	request.frame.op = KIOC_OP_REPLY;
	request.frame.arg1 = KIOC_FRAME_SIZE;
	request.buffer = native;
	request.length = KIOC_FRAME_SIZE;
	request.direction = KOS_IPC_DESC_OUT;
	result = call(ioc_handle, &request);
	if (result != KIOC_FRAME_SIZE || native[0] != expected ||
	    native[1] != 0u || native[2] != reply_length) {
		return result < 0 ? result : -KOS_EIO;
	}
	memcpy(reply_data, &native[3], reply_length);
	return 0;
}

static int read_page(unsigned char page, unsigned char *reply)
{
	return ioc_command(IOC_CMD_HID_STATUS, &page, 1u, IOC_RSP_HID_STATUS,
			   reply, HID_PAGE_LEN);
}

/* Both pages, or a negative error: -KOS_ENOSYS when the controller does not
 * know HID_STATUS, -KOS_ENOTSUP when it has no page 6 (it answers with page 0
 * instead, which the first byte gives away). */
static int read_pages(void)
{
	unsigned char page = HID_PAGE_OTHER;
	int result = read_page(HID_PAGE_STATUS, page0);

	if (result < 0) {
		return result;
	}
	result = read_page(HID_PAGE_GAMEPAD, page6);
	if (result < 0) {
		return result;
	}
	if (page6[P6_PAGE] != HID_PAGE_GAMEPAD) {
		return -KOS_ENOTSUP;
	}
	/* Page 7 is optional: firmware without it answers page 0, whose
	 * length does not match, and the line is simply left out. */
	page7[P7_PAGE] = 0u;
	have_page7 = (unsigned char)(ioc_command(IOC_CMD_HID_STATUS, &page, 1u,
						 IOC_RSP_HID_STATUS, page7,
						 HID_OTHER_LEN) == 0 &&
				     page7[P7_PAGE] == HID_PAGE_OTHER);
	return 0;
}

static void report_error(int result)
{
	if (result == -KOS_ENOSYS) {
		console_puts("padstat: the controller does not answer HID_STATUS\r\n");
	} else if (result == -KOS_ENOTSUP) {
		console_puts("padstat: controller firmware has no HID_STATUS page 6\r\n");
	} else {
		printf("padstat: HID_STATUS failed (%d)\r\n", result);
	}
}

static void print_other_line(void)
{
	const unsigned char *r = &page7[P7_LAST];

	if (page7[P7_ADDR] == 0u) {
		console_puts("Other HID: none\r\n");
		return;
	}
	printf("Other HID: addr %02x inst %02x reports %04x len %02x: %02x %02x %02x %02x %02x %02x %02x %02x\r\n",
	       page7[P7_ADDR], page7[P7_INST], le16(&page7[P7_REPORTS]),
	       page7[P7_LEN], r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
}

/* The same interface decoded as a mouse: running totals since the
 * controller started (they wrap), and the buttons held now. */
static void print_mouse_line(void)
{
	printf("Mouse: x %d  y %d  wheel %d  buttons %02x\r\n",
	       (int)le16(&page7[P7_X]), (int)le16(&page7[P7_Y]),
	       (int)le16(&page7[P7_WHEEL]), page7[P7_BUTTONS]);
}

static const char *controller_name(unsigned char status)
{
	switch (status) {
	case 0u: return "not started";
	case 1u: return "ready";
	case 2u: return "SPI error";
	case 3u: return "bad revision";
	case 4u: return "init failed";
	default: return "?";
	}
}

static const char *speed_name(unsigned char speed)
{
	switch (speed) {
	case 0u: return "full";
	case 1u: return "low";
	default: return "-";
	}
}

/* bInterfaceProtocol of the last HID interface mounted. */
static const char *proto_name(unsigned char proto)
{
	switch (proto) {
	case 0u: return "none";
	case 1u: return "keyboard";
	case 2u: return "mouse";
	case 0xFFu: return "never";
	default: return "?";
	}
}

static void print_pad(unsigned char slot, const unsigned char *pad)
{
	if (pad[PAD_ADDR] == 0u) {
		printf("Pad %u: empty (last reports %04x, len %02x)\r\n",
		       slot, le16(&pad[PAD_REPORTS]), pad[PAD_LEN]);
		return;
	}
	printf("Pad %u: addr %02x inst %02x arm %02x reports %04x len %02x latch %02x\r\n",
	       slot, pad[PAD_ADDR], pad[PAD_INST], pad[PAD_ARM],
	       le16(&pad[PAD_REPORTS]), pad[PAD_LEN], pad[PAD_LATCH]);
}

static void print_verdict(void)
{
	const unsigned char *pad = &page6[P6_PAD0];
	unsigned char proto = page6[P6_LAST_HID_PROTO];

	if (page0[P0_STATUS] != 1u) {
		console_puts("Verdict: USB host controller is not ready.\r\n");
		return;
	}
	if (proto == 2u) {
		printf("Verdict: a boot mouse enumerated at addr %02x through the hub.\r\n",
		       page6[P6_LAST_HID_ADDR]);
	}
	if (pad[PAD_ADDR] == 0u) {
		pad = &page6[P6_PAD1];
	}
	if (pad[PAD_ADDR] != 0u) {
		if (pad[PAD_ARM] != 1u) {
			console_puts("Verdict: F310 bound, but its first report request was refused.\r\n");
		} else if (le16(&pad[PAD_REPORTS]) == 0u) {
			console_puts("Verdict: F310 bound and armed; waiting for its first report.\r\n");
		} else {
			console_puts("Verdict: F310 reports are arriving.\r\n");
		}
		return;
	}
	if (le16(&page6[P6_LAST_VID]) == F310_VID &&
	    le16(&page6[P6_LAST_PID]) == F310_DIRECTINPUT_PID) {
		console_puts("Verdict: F310 seen in D mode, but no pad slot was free.\r\n");
	} else if (page6[P6_DEVICES] == 0u) {
		console_puts("Verdict: no USB device is configured.\r\n");
	} else if (proto != 2u) {
		console_puts("Verdict: no F310 in D mode (check the back switch).\r\n");
	}
}

static void print_report(void)
{
	printf("Controller: %s, revision %02x\r\n",
	       controller_name(page0[P0_STATUS]), page0[P0_REVISION]);
	printf("Devices %02x  mounts %02x  unmounts %02x\r\n",
	       page6[P6_DEVICES], page6[P6_MOUNTS], page6[P6_UNMOUNTS]);
	printf("Last device: addr %02x  VID:PID %04x:%04x\r\n",
	       page6[P6_LAST_ADDR], le16(&page6[P6_LAST_VID]),
	       le16(&page6[P6_LAST_PID]));
	printf("HID mounts %02x  last HID addr %02x  protocol %02x (%s)\r\n",
	       page6[P6_HID_MOUNTS], page6[P6_LAST_HID_ADDR],
	       page6[P6_LAST_HID_PROTO], proto_name(page6[P6_LAST_HID_PROTO]));
	if (page0[P0_KBD_ADDR] == 0u) {
		console_puts("Keyboard: none\r\n");
	} else {
		printf("Keyboard: addr %02x  speed %02x (%s)  reports %04x\r\n",
		       page0[P0_KBD_ADDR], page0[P0_KBD_SPEED],
		       speed_name(page0[P0_KBD_SPEED]),
		       le16(&page0[P0_KBD_REPORTS]));
	}
	print_pad(0u, &page6[P6_PAD0]);
	print_pad(1u, &page6[P6_PAD1]);
	if (have_page7 != 0u) {
		print_other_line();
		print_mouse_line();
	}
	print_verdict();
}

/* One line, under the console's 85 columns. */
static void print_watch_line(void)
{
	printf("dev %02x m %02x u %02x last %02x %04x:%04x hid %02x %s kbd %02x %s p0 %02x/%04x p1 %02x/%04x\r\n",
	       page6[P6_DEVICES], page6[P6_MOUNTS], page6[P6_UNMOUNTS],
	       page6[P6_LAST_ADDR], le16(&page6[P6_LAST_VID]),
	       le16(&page6[P6_LAST_PID]), page6[P6_LAST_HID_ADDR],
	       proto_name(page6[P6_LAST_HID_PROTO]), page0[P0_KBD_ADDR],
	       speed_name(page0[P0_KBD_SPEED]),
	       page6[P6_PAD0 + PAD_ADDR], le16(&page6[P6_PAD0 + PAD_REPORTS]),
	       page6[P6_PAD1 + PAD_ADDR], le16(&page6[P6_PAD1 + PAD_REPORTS]));
}

/* The pad report counts move on every report, so they are left out of the
 * change test; the line still shows them when it prints. */
static unsigned char is_report_count(unsigned char offset)
{
	return (unsigned char)(offset == P6_PAD0 + PAD_REPORTS ||
			       offset == P6_PAD0 + PAD_REPORTS + 1u ||
			       offset == P6_PAD1 + PAD_REPORTS ||
			       offset == P6_PAD1 + PAD_REPORTS + 1u);
}

/* Page 0 counts only for the fields shown: the rest is the keyboard report
 * count, the last keyboard report and the diagnostic probes. */
static unsigned char changed(void)
{
	unsigned char i;

	if (page0[P0_STATUS] != last0[P0_STATUS] ||
	    page0[P0_KBD_ADDR] != last0[P0_KBD_ADDR] ||
	    page0[P0_KBD_SPEED] != last0[P0_KBD_SPEED]) {
		return 1u;
	}
	for (i = 0u; i < HID_PAGE_LEN; i++) {
		if (is_report_count(i) == 0u && page6[i] != last6[i]) {
			return 1u;
		}
	}
	return 0u;
}

/* The keyboard interface's report count and its last report, raw.  Reports
 * arrive faster than the poll, so the count can jump by more than one. */
static void print_kbd_report_line(void)
{
	const unsigned char *r = &page0[P0_LAST_REPORT];

	printf("kbd reports %04x: %02x %02x %02x %02x %02x %02x %02x %02x\r\n",
	       le16(&page0[P0_KBD_REPORTS]), r[0], r[1], r[2], r[3],
	       r[4], r[5], r[6], r[7]);
}

static int watch(void)
{
	int timer = timer_open();
	int result;
	unsigned char first = 1u;
	char key;

	if (timer < 0) {
		printf("padstat: /dev/timer unavailable (%d)\r\n", timer);
		return 2;
	}
	console_puts("Watching USB/HID state; any key stops.\r\n");
	for (;;) {
		result = read_pages();
		if (result < 0) {
			report_error(result);
			return 2;
		}
		/* Every report the keyboard interface delivers, so moving a mouse
		 * that shares the interface shows up here even with no key down. */
		if (first == 0u &&
		    le16(&page0[P0_KBD_REPORTS]) != le16(&last0[P0_KBD_REPORTS])) {
			print_kbd_report_line();
			console_flush();
		}
		if (first == 0u && have_page7 != 0u &&
		    page7[P7_ADDR] != last7[P7_ADDR]) {
			print_other_line();
			console_flush();
		}
		if (first == 0u && have_page7 != 0u &&
		    memcmp(&page7[P7_BUTTONS], &last7[P7_BUTTONS],
			   HID_OTHER_LEN - P7_BUTTONS) != 0) {
			print_mouse_line();
			console_flush();
		}
		memcpy(last7, page7, HID_OTHER_LEN);
		if (first != 0u || changed() != 0u) {
			print_watch_line();
			console_flush();
			first = 0u;
		}
		memcpy(last0, page0, HID_PAGE_LEN);
		memcpy(last6, page6, HID_PAGE_LEN);
		if (console_read(&key, 1u) > 0) {
			return 0;
		}
		if (timer_wait_us(timer, WATCH_PERIOD_US) < 0) {
			console_puts("padstat: /dev/timer stopped\r\n");
			return 2;
		}
	}
}

/* MAX3421E HRSL result code, the low nibble. */
static const char *hrsl_name(unsigned char hrsl)
{
	static const char *const names[16] = {
		"success", "busy", "bad req", "undef", "NAK", "STALL",
		"toggle", "wrong PID", "bad BC", "PID err", "pkt err",
		"CRC", "K err", "J err", "timeout", "babble"
	};

	return names[hrsl & 0x0Fu];
}

/* Page 1 (diagnostic firmware): host controller and enumeration milestones.
 * The first milestone that reads zero is where enumeration stopped. */
static void print_page1(const unsigned char *p)
{
	printf("MODE %02x  HRSL %02x  last HRSL %02x (%s)  HIRQ %02x  mounted map %02x\r\n",
	       p[10], p[11], p[24], hrsl_name(p[24]), p[9], p[13]);
	printf("attach %02x  remove %02x  dev desc %02x  cfg desc %02x\r\n",
	       p[14], p[15], p[16], p[17]);
	printf("enum state %02x  enum fails %02x  ctrl rejects %02x\r\n",
	       p[18], p[19], p[20]);
	printf("HXFRDN %02x  xfer done %02x  ep null %02x  setups/xfers %02x\r\n",
	       p[21], p[22], p[23], p[25]);
}

/* Page 5 (diagnostic firmware): the HID class driver's set-config chain,
 * SET_IDLE -> SET_PROTOCOL (boot interfaces only) -> GET_DESCRIPTOR(report)
 * -> mount.  State is the step last entered, request the bRequest it
 * answered (0A SET_IDLE, 0B SET_PROTOCOL, 06 GET_DESCRIPTOR) and result its
 * xfer_result_t (0 success, 1 failed, 2 stalled, 3 timeout). */
static void print_page5(const unsigned char *p)
{
	printf("open %02x  set config %02x  steps %02x  state %02x  itf %02x\r\n",
	       p[1], p[2], p[3], p[4], p[5]);
	printf("last request %02x  result %02x  mount %02x  mount cb %02x\r\n",
	       p[6], p[7], p[8], p[9]);
	printf("HID mounts %02x  addr %02x  inst %02x  protocol %02x  arm %02x\r\n",
	       p[10], p[11], p[12], p[13], p[14]);
	printf("report cb %02x  addr %02x  busy %02x\r\n", p[23], p[24], p[18]);
}

/* Page 4 (diagnostic firmware): the behind-hub enumeration entry. */
static void print_page4(const unsigned char *p)
{
	printf("hub enum calls %02x  hub %02x port %02x  ret %02x  enumerating %02x\r\n",
	       p[1], p[2], p[3], p[4], p[5]);
	printf("defers %02x  completes %02x  attach hub %02x port %02x\r\n",
	       p[6], p[7], p[8], p[9]);
	printf("endpoints: alloc fails %02x  used %02x of %02x\r\n",
	       p[16], p[17], p[18]);
}

/* /P n: one page as raw bytes, with the fields of pages 1, 4 and 5 named.
 * The normal firmware answers pages 1-5 with page 0.  Byte 0 of pages 1-6 is
 * the page number, but page 0's byte 0 is the controller status, and "ready"
 * is 1 too; so a page that matches page 0 through the keyboard address
 * (status, revision, the reserved probe bytes, device count) is page 0.  A
 * real page 1 cannot: its bytes 1-4 are task and interrupt counters that move
 * between the two reads. */
static int dump_page(unsigned char page)
{
	unsigned char reply[HID_PAGE_LEN];
	unsigned char i;
	int result = read_page(page, reply);

	if (result == 0 && page != HID_PAGE_STATUS) {
		result = read_page(HID_PAGE_STATUS, page0);
	}
	if (result < 0) {
		report_error(result);
		return 2;
	}
	printf("Page %02x:", page);
	for (i = 0u; i < HID_PAGE_LEN; i++) {
		if ((i & 0x0Fu) == 0u) {
			printf("\r\n  %02x:", i);
		}
		printf(" %02x", reply[i]);
	}
	console_puts("\r\n");
	if (page != HID_PAGE_STATUS &&
	    (reply[0] != page || memcmp(reply, page0, P0_KBD_ADDR + 1u) == 0)) {
		console_puts("Not this page: the controller runs the normal firmware.\r\n");
		return 0;
	}
	if (page == 1u) {
		print_page1(reply);
	} else if (page == 4u) {
		print_page4(reply);
	} else if (page == 5u) {
		print_page5(reply);
	}
	return 0;
}

static void usage(void)
{
	console_puts("usage: padstat [/w | /p n]\r\n");
	console_puts("  /w    watch: print a line on every USB/HID change\r\n");
	console_puts("  /p n  dump HID_STATUS page n (1-5 need the diagnostic firmware)\r\n");
}

static int open_ioc(void)
{
	unsigned char type = 0u;
	int result = bind("/dev/ioc", 0u, &type);

	if (result < 0) {
		return result;
	}
	if (type != KOS_TYPE_IOC) {
		(void)unbind((unsigned char)result);
		return -KOS_ENODEV;
	}
	ioc_handle = (unsigned char)result;
	return 0;
}

int main(int argc, char **argv)
{
	unsigned char watching = 0u;
	unsigned char page = 0xFFu;
	int status;
	int result;
	int i;

	for (i = 0; i < argc; i++) {
		const char *arg = argv[i];
		char option = arg[1];

		if ((arg[0] != '/' && arg[0] != '-') || option == '\0' ||
		    arg[2] != '\0') {
			usage();
			return 1;
		}
		if (option == 'w' || option == 'W') {
			watching = 1u;
		} else if ((option == 'p' || option == 'P') && i + 1 < argc &&
			   argv[i + 1][0] >= '0' && argv[i + 1][0] <= '6' &&
			   argv[i + 1][1] == '\0') {
			page = (unsigned char)(argv[++i][0] - '0');
		} else {
			usage();
			return 1;
		}
	}
	if (watching != 0u && page != 0xFFu) {
		usage();
		return 1;
	}

	console_puts("PadStat - passive USB/HID status (HID_STATUS pages 0 and 6)\r\n");
	result = open_ioc();
	if (result < 0) {
		printf("padstat: /dev/ioc unavailable (%d)\r\n", result);
		return 2;
	}

	if (watching != 0u) {
		status = watch();
	} else if (page != 0xFFu) {
		status = dump_page(page);
	} else {
		result = read_pages();
		if (result < 0) {
			report_error(result);
			status = 2;
		} else {
			print_report();
			status = 0;
		}
	}
	(void)unbind(ioc_handle);
	return status;
}
