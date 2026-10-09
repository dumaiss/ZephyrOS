/* ZephyrOS IPC acceptance: the caller-buffer probe (test artifact).
 *
 * The ipc-selftest image boots this program; it measures the chained
 * 512-byte read and a single descriptor copy_out for every caller-buffer
 * location the correction-01 copy paths cover, then starts the promoted
 * conformance client (/SYSTEM/ipc.zex) and folds its verdict into its own
 * line.
 *
 * The five addresses are the correction's matrix:
 *
 *   0100h  caller window                      path A (two LDIRs, mode 11)
 *   3000h  primary bank, clear of the aperture path B (aperture LDIR)
 *   7000h  primary bank, clear of the aperture path B
 *   C000h  primary bank, clear of the aperture path B
 *   9000h  primary bank behind the aperture    path C (chunked gate)
 *
 * Path classification is done from the outside, by the simulator replay:
 * it sees which latches the copy resolver writes (a mode-10 write with
 * page 1 is path B, none at all in a call is path A, the bank-copy gate's
 * mode-10 writes with page 0 are path C) and it reads the resolver's path
 * counters out of the image.  This program only drives the protocol and
 * checks that the bytes arrive.
 *
 * The program is linked with its code above the buffer window
 * (LOWBASE=0x0300, CODEBASE=0x0344, NO_CRT0), so 0100h-02FFh is a plain
 * 512-byte buffer
 * with no code or data of this program in it: that is the only way a ZEX
 * can exercise the caller window itself.
 */

#include "zos.h"

#include "ipc_test.h"

#define PROBE_MEASURE_N1 2u
#define PROBE_MEASURE_N2 8u

#define PROBE_CALLER_WINDOW 0x0100u
#define PROBE_CALLER_HIGH1 0x3000u
#define PROBE_CALLER_HIGH2 0x7000u
#define PROBE_CALLER_HIGH3 0xC000u
#define PROBE_CALLER_BEHIND 0x9000u

/* The same pattern the client's console probes use: never a marker byte. */
#define PROBE_PATTERN_BASE 0x80u
#define PROBE_PATTERN_MASK 0x3Fu

static struct zos_ipc_request req;

static uint8_t echo_handle;
static uint8_t vfs_handle;
static uint8_t ioc_handle;
static uint8_t con_handle;
static uint8_t irqx_handle;

static unsigned int checks;
static unsigned int failures;

static const char client_path[] = "/SYSTEM/ipc.zex";

static void check(const char *label, int ok)
{
	++checks;
	if (!ok) {
		++failures;
		zos_puts("ipccopy: FAIL ");
		zos_puts(label);
		zos_puts("\r\n");
	}
}

static void report_u16(const char *label, unsigned int value)
{
	char digits[6];
	unsigned int count = 0u;

	zos_puts(label);
	do {
		digits[count++] = (char)('0' + (value % 10u));
		value /= 10u;
	} while (value != 0u && count < 5u);
	while (count != 0u) {
		zos_putc(digits[--count]);
	}
}

/* --- frame plumbing ----------------------------------------------------- */

static void frame_prepare(uint8_t op, uint8_t flags)
{
	req.frame.op = op;
	req.frame.flags = flags;
	req.frame.desc = ZOS_IPC_DESC_NONE;
	req.frame.arg0 = 0u;
	req.frame.arg1 = 0u;
	req.frame.status = 0;
	memset(req.frame.payload, 0, ZOS_IPC_PAYLOAD_SIZE);
	req.buffer = (void *)0;
	req.length = 0u;
	req.direction = ZOS_IPC_DESC_IN;
}

static int call(uint8_t handle, uint8_t op, uint8_t flags, void *buffer,
		unsigned int length, unsigned char direction)
{
	frame_prepare(op, flags);
	req.buffer = buffer;
	req.length = length;
	req.direction = direction;
	return zos_ipc_call(handle, &req);
}

/* The chained read: vfs -> fat -> ioc, with the caller descriptor
 * travelling down the chain and ioc copying the block straight out. */
static int read_chain(void *buffer)
{
	frame_prepare(IPC_OP_VFS_READ, 0u);
	req.frame.arg0 = IPC_OBJECT_HANDLE;
	req.frame.arg1 = 0u;
	req.frame.payload[0] = IPC_WORD_LO(IPC_BLOCK_BYTES);
	req.frame.payload[1] = IPC_WORD_HI(IPC_BLOCK_BYTES);
	req.buffer = buffer;
	req.length = IPC_BLOCK_BYTES;
	req.direction = ZOS_IPC_DESC_OUT;
	return zos_ipc_call(vfs_handle, &req);
}

/* The single copy_out: ioc alone, one descriptor copy and nothing else. */
static int read_single(void *buffer)
{
	frame_prepare(IPC_OP_IOC_READ, 0u);
	req.frame.arg0 = IPC_OBJECT_HANDLE;
	req.frame.arg1 = 0u;
	req.frame.payload[0] = IPC_WORD_LO(IPC_BLOCK_BYTES);
	req.frame.payload[1] = IPC_WORD_HI(IPC_BLOCK_BYTES);
	req.buffer = buffer;
	req.length = IPC_BLOCK_BYTES;
	req.direction = ZOS_IPC_DESC_OUT;
	return zos_ipc_call(ioc_handle, &req);
}

/* A forged descriptor id, captured at the case's own address: the copy is
 * refused before any path is chosen, whatever the address would select. */
static int forged_at(void *buffer)
{
	frame_prepare(IPC_OP_CON_PROBE, 0u);
	req.frame.payload[0] = 2u;
	req.frame.payload[1] = 0u;
	req.buffer = buffer;
	req.length = 16u;
	req.direction = ZOS_IPC_DESC_IN;
	return zos_ipc_call(con_handle, &req);
}

/* One con.WRITE whose target is the service's deliberate out-of-range
 * buffer: the only case that selects path C for a reason on the service
 * side.  The console bytes are checked by the replay. */
static int con_slow_one(void)
{
	frame_prepare(IPC_OP_CON_WRITE, IPC_FRAME_SLOW_IO);
	req.frame.arg0 = 0u;
	req.frame.arg1 = 16u;
	req.buffer = (void *)PROBE_CALLER_WINDOW;
	req.length = 16u;
	req.direction = ZOS_IPC_DESC_IN;
	return zos_ipc_call(con_handle, &req);
}

/* Path B with the timer armed: the delivery has to land inside the copy
 * primitive's aperture window, and the reply says how many copies ran and
 * what the service's buffer summed to. */
static int irqx_copies(void *buffer)
{
	frame_prepare(IPC_OP_IRQX_COPY, 0u);
	req.frame.arg1 = 4u;
	req.buffer = buffer;
	req.length = IRQX_COPY_BYTES;
	req.direction = ZOS_IPC_DESC_IN;
	return zos_ipc_call(irqx_handle, &req);
}

/* --- pattern helpers ---------------------------------------------------- */

static int ioc_pattern_ok(const uint8_t *buffer, unsigned int length)
{
	unsigned int index;

	for (index = 0u; index < length; ++index) {
		if (buffer[index] != (uint8_t)(
			    IPC_IOC_PATTERN_BASE +
			    (uint8_t)((IPC_OBJECT_HANDLE + (uint8_t)index) &
				      IPC_IOC_PATTERN_MASK))) {
			return 0;
		}
	}
	return 1;
}

static void fill_probe_pattern(unsigned int address)
{
	unsigned int index;

	for (index = 0u; index < IPC_BLOCK_BYTES; ++index) {
		((uint8_t *)address)[index] = (uint8_t)(
			PROBE_PATTERN_BASE + (index & PROBE_PATTERN_MASK));
	}
}

/* The 16-bit sum of one buffer, matching the irqx service's own sum over
 * what landed in its buffer. */
static uint16_t probe_pattern_sum(unsigned int address)
{
	unsigned int index;
	uint16_t sum = 0u;

	for (index = 0u; index < IPC_BLOCK_BYTES; ++index) {
		sum = (uint16_t)(sum + ((uint8_t *)address)[index]);
	}
	return sum;
}

/* --- phase reporting ---------------------------------------------------- */

/* One marker per phase boundary, the client's wire format ("ipc: Xs").
 * The codes are the test's PHASE_NAMES table: A-E are the chained reads,
 * P-T the single copy_outs, U the service-side slow case and V the
 * IRQ-during-path-B case, in this order. */
static void phase_mark(char code, char suffix)
{
	zos_puts("ipc: ");
	zos_putc(code);
	zos_putc(suffix);
	zos_puts("\r\n");
}

#define MEASURE(name, body) \
	do { \
		unsigned int measure_index; \
		phase_mark(name, 'a'); \
		for (measure_index = 0u; measure_index < PROBE_MEASURE_N1; \
		     ++measure_index) { \
			body; \
		} \
		phase_mark(name, 'b'); \
		for (measure_index = 0u; measure_index < PROBE_MEASURE_N2; \
		     ++measure_index) { \
			body; \
		} \
		phase_mark(name, 'c'); \
	} while (0)

/* Chained read plus a single copy_out at one address, both measured, with
 * the bytes checked and the forged-descriptor refusal proved at the same
 * address (the refusal happens before any path is selected). */
static void case_at(unsigned int address, char read_code, char single_code,
		    const char *label)
{
	uint8_t *buffer = (uint8_t *)address;
	int rc = -1;

	MEASURE(read_code, rc = read_chain(buffer));
	check(label, rc == (int)IPC_BLOCK_BYTES &&
		     ioc_pattern_ok(buffer, IPC_BLOCK_BYTES));

	MEASURE(single_code, rc = read_single(buffer));
	check(label, rc == (int)IPC_BLOCK_BYTES &&
		     ioc_pattern_ok(buffer, IPC_BLOCK_BYTES));
	check("forged-refused", forged_at(buffer) == -ZOS_EINVAL);
}

void ipc_copy_entry(void)
{
	int rc;
	int copies;
	uint16_t expect;

	zos_puts("ipccopy: probe OK\r\n");

	echo_handle = (uint8_t)zos_ipc_lookup(IPC_CLASS_ECHO, 0u);
	vfs_handle = (uint8_t)zos_ipc_lookup(IPC_CLASS_VFS, 0u);
	ioc_handle = (uint8_t)zos_ipc_lookup(IPC_CLASS_IOC, 0u);
	con_handle = (uint8_t)zos_ipc_lookup(IPC_CLASS_CON, 0u);
	irqx_handle = (uint8_t)zos_ipc_lookup(IPC_CLASS_IRQX, 0u);
	check("lookup", vfs_handle != 0u && ioc_handle != 0u &&
			con_handle != 0u && irqx_handle != 0u);

	case_at(PROBE_CALLER_WINDOW, 'A', 'P', "window-bytes");
	case_at(PROBE_CALLER_HIGH1, 'B', 'Q', "high1-bytes");
	case_at(PROBE_CALLER_HIGH2, 'C', 'R', "high2-bytes");
	case_at(PROBE_CALLER_HIGH3, 'D', 'S', "high3-bytes");
	case_at(PROBE_CALLER_BEHIND, 'E', 'T', "behind-bytes");

	/* Correction 01 to architecture 36.8: the caller buffer window also
	 * covers the common page (E000h-FFFFh), which every mapping shows at
	 * the same address.  ECHO_DESC only reports the captured id, so no
	 * kernel byte is read or written by this check. */
	rc = call(echo_handle, IPC_OP_ECHO_DESC, 0u, (void *)0xE100u, 16u,
		  ZOS_IPC_DESC_IN);
	check("common-accepted", rc >= 0 && rc != (int)ZOS_IPC_DESC_NONE);

	/* The service-side slow case: the caller buffer is in the window
	 * (path A as an address), so only the service's out-of-range target
	 * can select path C. */
	fill_probe_pattern(PROBE_CALLER_WINDOW);
	MEASURE('U', rc = con_slow_one());
	check("slow-return", rc == 16);

	/* Path B with the timer armed and the delivery inside the window:
	 * the caller buffer is in the primary bank clear of the aperture, so
	 * every copy opens the mode-10 window. */
	fill_probe_pattern(PROBE_CALLER_HIGH1);
	expect = probe_pattern_sum(PROBE_CALLER_HIGH1);
	MEASURE('V', copies = irqx_copies((void *)PROBE_CALLER_HIGH1));
	check("apcopy-status", copies == 0);
	check("apcopy-copies", req.frame.payload[3] == 4u);
	check("apcopy-sum", IPC_FRAME_WORD(req.frame.payload, 0u) == expect);
	check("apcopy-irq", req.frame.payload[2] >= 1u);

	/* The full promoted conformance suite, as a child process. */
	rc = zos_exec(client_path, (const char *)0);
	check("client-status", rc == 0);

	report_u16("ipccopy: ", checks - failures);
	report_u16("/", checks);
	zos_puts(" checks\r\n");
	if (failures == 0u) {
		zos_puts("ipccopy: PASS\r\n");
	} else {
		zos_puts("ipccopy: FAIL\r\n");
	}
	zex_exit(failures == 0u ? 0u : 1u);
}
