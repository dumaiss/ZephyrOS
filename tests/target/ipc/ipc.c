/* KonaOS IPC acceptance: the client program (test artifact).
 *
 * The EXE primary the ipc-selftest image boots.  It is an ordinary SDK
 * program: it discovers the synthetic endpoints through the SDK's cached
 * lookup, marshals frames and one buffer descriptor per call through the
 * SDK's call wrapper, runs the promoted package-35 conformance checks,
 * and prints one marker line per measured phase so the simulator replay
 * can measure the spans between them.
 *
 * All measurement is done by the replay from the outside (T-states, latch
 * intervals, IRQ delivery, stack high-water marks); this program drives
 * the protocols and checks the results.  Its final line is the
 * user-visible verdict.
 */

#include "kos.h"

#include "kos_internal.h"

#include "ipc_test.h"

#define MEASURE_N1 8u
#define MEASURE_N2 24u

#define PATTERN_CON_BASE 0x80u /* bytes above ASCII: never a marker byte */
#define PATTERN_MASK 0x3Fu

static struct kos_ipc_request req;
static struct kos_ipc_call_args raw_block;
static struct kos_ipc_call_req raw_req;

static uint8_t source[IPC_BLOCK_BYTES];
static uint8_t target[IPC_BLOCK_BYTES];

static uint8_t echo_handle;
static uint8_t con_handle;
static uint8_t vfs_handle;
static uint8_t fat_handle;
static uint8_t ioc_handle;
static uint8_t irqx_handle;

static unsigned int checks;
static unsigned int failures;

static const char fs_path[] = "/system/ipc.exe";

static void check(const char *label, int ok)
{
	++checks;
	if (!ok) {
		++failures;
		console_puts("ipc: FAIL ");
		console_puts(label);
		console_puts("\r\n");
	}
}

/* --- frame plumbing ----------------------------------------------------- */

static void frame_prepare(uint8_t op, uint8_t flags)
{
	req.frame.op = op;
	req.frame.flags = flags;
	req.frame.desc = KOS_IPC_DESC_NONE;
	req.frame.arg0 = 0u;
	req.frame.arg1 = 0u;
	req.frame.status = 0;
	memset(req.frame.payload, 0, KOS_IPC_PAYLOAD_SIZE);
	req.buffer = (void *)0;
	req.length = 0u;
	req.direction = KOS_IPC_DESC_IN;
}

static int dispatch(uint8_t handle)
{
	return call(handle, &req);
}

static int ipc_call(uint8_t handle, uint8_t op, uint8_t flags, void *buffer,
		unsigned int length, unsigned char direction)
{
	frame_prepare(op, flags);
	req.buffer = buffer;
	req.length = length;
	req.direction = direction;
	return dispatch(handle);
}

static int raw_ipc(unsigned char number, void *block)
{
	raw_req.number = number;
	raw_req.block = block;
	return kos_ipc_raw_call(&raw_req);
}

/* --- pattern helpers ---------------------------------------------------- */

static void fill_console_pattern(void)
{
	unsigned int index;

	for (index = 0u; index < IPC_BLOCK_BYTES; ++index) {
		source[index] = (uint8_t)(PATTERN_CON_BASE +
					  (index & PATTERN_MASK));
	}
}

static int ioc_pattern_ok(const uint8_t *buffer, unsigned int start,
			  unsigned int length)
{
	unsigned int index;

	for (index = 0u; index < length; ++index) {
		uint16_t at = (uint16_t)(start + index);

		if (buffer[index] != (uint8_t)(
			    IPC_IOC_PATTERN_BASE +
			    (uint8_t)((IPC_OBJECT_HANDLE + at) &
				      IPC_IOC_PATTERN_MASK))) {
			return 0;
		}
	}
	return 1;
}

static int same_bytes(const uint8_t *a, const uint8_t *b,
		      unsigned int length)
{
	unsigned int index;

	for (index = 0u; index < length; ++index) {
		if (a[index] != b[index]) {
			return 0;
		}
	}
	return 1;
}

/* --- measured operations ------------------------------------------------ */

static int op_echo_exchange(void)
{
	frame_prepare(IPC_OP_ECHO_EXCHANGE, 0u);
	req.frame.payload[0] = 0x11u;
	return dispatch(echo_handle);
}

static int op_echo_null(void)
{
	return ipc_call(echo_handle, IPC_OP_ECHO_NULL, 0u, (void *)0, 0u, 0u);
}

static int con_write(unsigned int offset, unsigned int length)
{
	frame_prepare(IPC_OP_CON_WRITE, 0u);
	req.frame.arg0 = (uint8_t)offset;
	req.frame.arg1 = (uint16_t)length;
	req.buffer = source;
	req.length = IPC_BLOCK_BYTES;
	req.direction = KOS_IPC_DESC_IN;
	return dispatch(con_handle);
}

/* One con.WRITE with an explicit descriptor length: the check that a
 * slice past the captured descriptor, and a slice past the driver's own
 * buffer, are both refused with -KOS_EINVAL. */
static int con_slice(unsigned int descriptor, unsigned int offset)
{
	frame_prepare(IPC_OP_CON_WRITE, 0u);
	req.frame.arg0 = (uint8_t)offset;
	req.frame.arg1 = 4u;
	req.buffer = source;
	req.length = descriptor;
	req.direction = KOS_IPC_DESC_IN;
	return dispatch(con_handle);
}

static int op_con1(void) { return con_write(0u, 1u); }
static int op_con16(void) { return con_write(0u, 16u); }
static int op_con64(void) { return con_write(0u, 64u); }
static int op_con256(void) { return con_write(0u, 256u); }
static int op_conoffset(void) { return con_write(8u, 4u); }

static int op_console1(void)
{
	return console_write((char *)source, 1u);
}

static int op_console256(void)
{
	return console_write((char *)source, 256u);
}

static int vfs_read(unsigned int offset, unsigned int length, uint8_t flags)
{
	frame_prepare(IPC_OP_VFS_READ, flags);
	req.frame.arg0 = IPC_OBJECT_HANDLE;
	req.frame.arg1 = (uint16_t)offset;
	req.frame.payload[0] = IPC_WORD_LO(length);
	req.frame.payload[1] = IPC_WORD_HI(length);
	req.buffer = target;
	req.length = IPC_BLOCK_BYTES;
	req.direction = KOS_IPC_DESC_OUT;
	return dispatch(vfs_handle);
}

static int op_vfs_direct(void) { return vfs_read(0u, IPC_BLOCK_BYTES, 0u); }
static int op_vfs_staged(void)
{
	return vfs_read(0u, IPC_BLOCK_BYTES, IPC_FRAME_STAGED);
}
static int op_vfs_offset(void) { return vfs_read(448u, 64u, 0u); }

/* The filesystem baseline on the same 512-byte chunk: the file is
 * reached through a bound handle and its protocol calls, so this is what
 * the resident chain is compared against (package 42b: there is no
 * special-cased direct path left - the M9 front end it named is gone). */
static int op_fs_read(void)
{
	static int fd;
	static unsigned char fd_valid;
	long rc;

	if (!fd_valid) {
		fd = open(fs_path, KFS_O_READ);
		if (fd < 0) {
			return fd;
		}
		fd_valid = 1u;
	}
	rc = seek((unsigned int)fd, 0, KFS_SEEK_SET);
	if (rc < 0) {
		return (int)rc;
	}
	return read((unsigned int)fd, target, IPC_BLOCK_BYTES);
}

static int irq_op(uint8_t op, unsigned int iterations)
{
	frame_prepare(op, 0u);
	req.frame.arg1 = (uint16_t)iterations;
	return dispatch(irqx_handle);
}

/* --- phase reporting ---------------------------------------------------- */

/* One marker per phase boundary.  The code is a single letter so the
 * replay can name the measured spans without carrying a string table in
 * the program (the caller window is 8 KiB and shared with libkos); the
 * test's PHASE_NAMES table is the authoritative decoding. */
static void phase_mark(char code, char suffix)
{
	console_puts("ipc: ");
	putchar(code);
	putchar(suffix);
	console_puts("\r\n");
}

static void report_u16(const char *label, unsigned int value)
{
	char digits[6];
	unsigned int count = 0u;

	console_puts(label);
	do {
		digits[count++] = (char)('0' + (value % 10u));
		value /= 10u;
	} while (value != 0u && count < 5u);
	while (count != 0u) {
		putchar(digits[--count]);
	}
}

#define MEASURE(name, N1, N2, body) \
	do { \
		unsigned int measure_index; \
		phase_mark(name, 'a'); \
		for (measure_index = 0u; measure_index < (N1); \
		     ++measure_index) { \
			body; \
		} \
		phase_mark(name, 'b'); \
		for (measure_index = 0u; measure_index < (N2); \
		     ++measure_index) { \
			body; \
		} \
		phase_mark(name, 'c'); \
	} while (0)

int main(int argc, char **argv)
{
	int rc;
	unsigned char type = 0u;
	unsigned char con_type = 0u;
	unsigned char vfs_type = 0u;
	unsigned char fat_type = 0u;
	unsigned char ioc_type = 0u;
	unsigned int index;

	(void)argc;
	(void)argv;
	console_puts("ipc selftest: client OK\r\n");

	/* Package 42b: every provider is reached through a bound namespace
	 * handle; the three host-built publications of the production
	 * drivers would resolve here too (the image's own set is below
	 * /dev/ipc). */
	echo_handle = (uint8_t)bind("/dev/ipc/echo", 0u, &type);
	check("bind-echo", echo_handle != 0u && type == KOS_TYPE_SERIAL);
	con_handle = (uint8_t)bind("/dev/ipc/con", 0u, &con_type);
	vfs_handle = (uint8_t)bind("/dev/ipc/vfs", 0u, &vfs_type);
	fat_handle = (uint8_t)bind("/dev/ipc/fat", 0u, &fat_type);
	ioc_handle = (uint8_t)bind("/dev/ipc/ioc", 0u, &ioc_type);
	irqx_handle = (uint8_t)bind("/dev/ipc/irqx", 0u, &type);
	check("bind-con", con_handle != 0u && con_type == KOS_TYPE_CONSOLE);
	check("bind-vfs", vfs_handle != 0u && vfs_type != 0u);
	check("bind-fat", fat_handle != 0u && fat_type != 0u);
	check("bind-ioc", ioc_handle != 0u && ioc_type == KOS_TYPE_IOC);
	check("bind-irqx", irqx_handle != 0u);
	check("bind-missing",
	      bind("/dev/ipc/nosuch", 0u, &type) == -KOS_ENOENT);
	check("bind-group",
	      bind("/dev/ipc", 0u, &type) == -KOS_EISDIR);

	fill_console_pattern();

	/* C3: the null call. */
	MEASURE('n', MEASURE_N1, MEASURE_N2, rc = op_echo_exchange());
	check("null-status", rc == 0x12);
	check("null-echo", req.frame.payload[1] == 0x11u);
	check("null-ring", op_echo_null() == KOS_IPC_FRAME_SIZE);

	/* The descriptor id the nucleus issued: none without a buffer, the
	 * captured id with one. */
	check("desc-none", ipc_call(echo_handle, IPC_OP_ECHO_DESC, 0u, (void *)0,
				0u, 0u) == KOS_IPC_DESC_NONE);
	check("desc-in", ipc_call(echo_handle, IPC_OP_ECHO_DESC, 0u, source, 16u,
			      KOS_IPC_DESC_IN) == 0);
	check("desc-out", ipc_call(echo_handle, IPC_OP_ECHO_DESC, 0u, source, 16u,
			       KOS_IPC_DESC_OUT) == 0);
	check("status-negative", ipc_call(echo_handle, 0x7Fu, 0u, (void *)0, 0u,
				      0u) == -KOS_EINVAL);
	check("handle-refused", ipc_call(0u, IPC_OP_ECHO_NULL, 0u, (void *)0, 0u,
				     0u) == -KOS_EINVAL);

	/* Structural refusals: the request never reaches the IPC layer. */
	raw_block.handle = echo_handle;
	raw_block.op = IPC_OP_ECHO_NULL;
	raw_block.flags = 0u;
	raw_block.frame_length = 0u;
	raw_block.buffer = 0u;
	raw_block.length = 0u;
	raw_block.direction = KOS_IPC_DESC_IN;
	raw_block.reserved = 0u;
	raw_block.status = 0;
	memset(&raw_block.frame, 0, KOS_IPC_FRAME_SIZE);
	check("frame-len-refused",
	      raw_ipc(KOS_SYSCALL_CALL, &raw_block) == -KOS_EINVAL);
	check("block-refused",
	      raw_ipc(KOS_SYSCALL_CALL, (void *)0x8000u) == -KOS_EINVAL);
	check("bind-block-refused",
	      raw_ipc(KOS_SYSCALL_BIND, (void *)0x8000u) == -KOS_EINVAL);

	/* C1/C3: con.WRITE at four sizes, one offset slice, and today's
	 * direct console path. */
	rc = op_con1();
	check("con1-status", rc == 1);
	rc = op_con16();
	check("con16-status", rc == 16);
	rc = op_con64();
	check("con64-status", rc == 64);
	MEASURE('c', MEASURE_N1, MEASURE_N2, rc = op_con256());
	check("con256-status", rc == 256);
	rc = op_conoffset();
	check("conoffset-status", rc == 4);
	check("conoffset-refused", con_slice(256u, 254u) == -KOS_EINVAL);
	check("conbound-refused", con_slice(IPC_BLOCK_BYTES, 254u) ==
			  -KOS_EINVAL);
	check("dir-refused", ipc_call(con_handle, IPC_OP_CON_WRITE, 0u,
				  source, 16u, KOS_IPC_DESC_OUT) ==
			     -KOS_EINVAL);
	check("con-poll", ipc_call(con_handle, IPC_OP_CON_POLL, 0u, (void *)0, 0u,
			       0u) >= 0);

	rc = op_console1();
	check("console1-status", rc == 1);
	MEASURE('o', MEASURE_N1, MEASURE_N2, rc = op_console256());
	check("console256-status", rc == 256);

	/* C1/C3/C5: the 512-byte chained read, direct and staged, an offset
	 * slice, and the repeat that must produce identical bytes. */
	MEASURE('r', MEASURE_N1, MEASURE_N2, rc = op_vfs_direct());
	check("read-status", rc == IPC_BLOCK_BYTES);
	check("read-pattern", ioc_pattern_ok(target, 0u, IPC_BLOCK_BYTES));
	MEASURE('s', MEASURE_N1, MEASURE_N2, rc = op_vfs_offset());
	check("readoffset-status", rc == 64);
	check("readoffset-pattern", ioc_pattern_ok(target + 448u, 448u, 64u));
	MEASURE('t', MEASURE_N1, MEASURE_N2, rc = op_vfs_staged());
	check("staged-status", rc == IPC_BLOCK_BYTES);
	check("staged-pattern", ioc_pattern_ok(target, 0u, IPC_BLOCK_BYTES));
	for (index = 0u; index < IPC_BLOCK_BYTES; ++index) {
		source[index] = target[index];
	}
	MEASURE('u', MEASURE_N1, MEASURE_N2, rc = op_vfs_direct());
	check("read2-status", rc == IPC_BLOCK_BYTES);
	check("conform-payload", same_bytes(target, source, IPC_BLOCK_BYTES));

	/* The filesystem baseline: one file read on a bound handle. */
	MEASURE('f', MEASURE_N1, MEASURE_N2, rc = op_fs_read());
	check("fsread-status", rc > 0);

	/* C7: re-entrancy is refused unless the endpoint declares it, and
	 * the depth limit is four. */
	check("reent-refused", ipc_call(echo_handle, IPC_OP_ECHO_SELF, 0u,
				    (void *)0, 0u, 0u) == -KOS_EBUSY);
	frame_prepare(IPC_OP_VFS_SELF, 0u);
	req.frame.arg0 = 3u;
	check("reent-allowed", dispatch(vfs_handle) == 0);
	frame_prepare(IPC_OP_VFS_SELF, 0u);
	req.frame.arg0 = 4u;
	check("depth-refused", dispatch(vfs_handle) == -KOS_ENOSPC);

	/* C2: forged descriptor ids and out-of-window buffers are refused. */
	check("capture-low-refused", ipc_call(con_handle, IPC_OP_CON_WRITE, 0u,
					  (void *)0x0040u, 16u,
					  KOS_IPC_DESC_IN) == -KOS_EINVAL);
	frame_prepare(IPC_OP_CON_WRITE, 0u);
	req.frame.arg1 = 16u;
	req.buffer = source;
	req.length = KOS_IPC_DESC_MAX + 1u;
	req.direction = KOS_IPC_DESC_IN;
	check("capture-length-refused", dispatch(con_handle) == -KOS_EINVAL);
	frame_prepare(IPC_OP_CON_PROBE, 0u);
	req.frame.payload[0] = 2u;
	req.frame.payload[1] = 0u;
	check("forged-refused", dispatch(con_handle) == -KOS_EINVAL);

	/* Nucleus -> driver upcalls: the exit notification and the
	 * executable-object stream (a nucleus-held object handle, no
	 * pathname and no raw caller address). */
	frame_prepare(IPC_OP_VFS_ON_EXIT, 0u);
	req.frame.arg0 = 42u;
	check("upcall-exit", dispatch(vfs_handle) == 42);
	frame_prepare(IPC_OP_VFS_EXEC, 0u);
	req.frame.arg0 = IPC_OBJECT_HANDLE;
	rc = dispatch(vfs_handle);
	check("upcall-exec", rc == IPC_STREAM_BYTES);

	/* C6: an IRQ owned by a driver.  The claim is released first, so
	 * the same timer delivery is seen unclaimed (the executive quiesces
	 * it), then re-claimed, so two deliveries exercise claim, release
	 * and the delivered-handler path while a driver call is in
	 * progress. */
	check("irq-release", irq_op(IPC_OP_IRQX_RELEASE, 0u) == 0);
	check("irq-unclaimed",
	      irq_op(IPC_OP_IRQX_SPIN, 0x0800u) == 0);
	check("irq-release-again",
	      irq_op(IPC_OP_IRQX_RELEASE, 0u) == -KOS_ENOENT);
	check("irq-reclaim", irq_op(IPC_OP_IRQX_CLAIM, 0u) == 0);
	check("irq-driver", irq_op(IPC_OP_IRQX_SPIN, 0x0800u) == 1);
	check("irq-count", irq_op(IPC_OP_IRQX_SPIN, 0x0800u) == 2);

	/* The client survived the IRQ-during-call, so the interrupted
	 * call's latch and stack were restored: prove it by using the
	 * substrate again after the deliveries. */
	check("post-irq-call", op_echo_null() == KOS_IPC_FRAME_SIZE);

	report_u16("ipc: ", checks - failures);
	report_u16("/", checks);
	console_puts(" checks\r\n");
	if (failures == 0u) {
		console_puts("ipc: PASS\r\n");
	} else {
		console_puts("ipc: FAIL\r\n");
	}
	/* The status is the verdict: the probe program that starts this one
	 * reads it back through SYS_EXEC and folds it into its own line. */
	return (int)(failures != 0u);
}
