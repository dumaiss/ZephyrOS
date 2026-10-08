/* ZephyrOS resident-service spike: the client program (test only).
 *
 * The ZEX v1 primary the spike image boots.  It is a client of the
 * nucleus IPC substrate, not of any kernel feature syscall: it discovers
 * endpoints through the evaluation lookup call, marshals bounded frames
 * through SDK-style stubs, runs the C1-C9 measurement phases, and prints
 * one marker line per phase so the simulator replay can time the spans
 * between them.
 *
 * All measurement is done by the replay from the outside (T-states,
 * latch intervals, IRQ delivery); this program only drives the protocols
 * and checks the results.  Its final line is the user-visible verdict.
 */

#include "zos.h"

#include "rsvc_test.h"
#include "zos/fs.h"
#include "zos/syscall.h"

struct rsvc_raw_req {
	unsigned char number;
	void         *block;
};

extern int rsvc_raw_call(struct rsvc_raw_req *req);

#define MEASURE_N1 8u
#define MEASURE_N2 24u

static struct rsvc_raw_req raw_req;
static struct rsvc_call_req call_req;
static struct rsvc_irq_req irq_req;
static struct rsvc_upcall_req up_req;
static struct rsvc_lookup_req look_req;
static struct rsvc_evidence ev;

static uint8_t echo_handle;
static uint8_t con_handle;
static uint8_t vfs_handle;

static uint8_t source[512];
static uint8_t target[512];
static uint16_t stacks[3];
static const char fs_path[] = "/SYSTEM/rsvc.zex";

static unsigned int checks;
static unsigned int failures;
static unsigned int phase_count;

#define PATTERN_CON_BASE 0x80u /* bytes above ASCII: never a marker byte */
#define PATTERN_IOC_BASE 0x40u
#define PATTERN_MASK 0x3Fu

static void check(const char *label, int ok)
{
	++checks;
	if (!ok) {
		++failures;
		zos_puts("rsvc: FAIL ");
		zos_puts(label);
		zos_puts("\r\n");
	}
}

static int raw(unsigned char number, void *block)
{
	raw_req.number = number;
	raw_req.block = block;
	return rsvc_raw_call(&raw_req);
}

static int endpoint_lookup(uint8_t class, uint8_t instance)
{
	look_req.class = class;
	look_req.instance = instance;
	look_req.handle = 0u;
	if (raw(ZOS_RSVC_SYSCALL_LOOKUP, &look_req) < 0) {
		return -1;
	}
	return look_req.handle ? (int)look_req.handle : -1;
}

static int call_prepare(uint8_t handle, uint8_t op, uint8_t frame_len)
{
	call_req.handle = handle;
	call_req.buf_dir = 0u;
	call_req.frame_len = frame_len;
	call_req.reserved = 0u;
	call_req.buf_addr = 0u;
	call_req.buf_len = 0u;
	call_req.frame.op = op;
	call_req.frame.flags = 0u;
	call_req.frame.desc = RSVC_DESC_NONE;
	call_req.frame.arg0 = 0u;
	call_req.frame.arg1 = 0u;
	call_req.frame.result = 0;
	call_req.result = 0;
	return 0;
}

static int call_run(unsigned char number, void *buffer, uint16_t length,
		    uint8_t direction)
{
	if (length != 0u) {
		call_req.buf_addr = (uint16_t)buffer;
		call_req.buf_len = length;
		call_req.buf_dir = direction;
	}
	return raw(number, &call_req);
}

/* Syscall argument blocks must live in the caller window (0000h-1FFFh),
 * so every block the client passes is a static here, never a stack slot. */
static uint16_t dst_block;

static int evidence_read(void)
{
	dst_block = (uint16_t)&ev;
	return raw(ZOS_RSVC_SYSCALL_EVIDENCE, &dst_block);
}

static int stacks_read(void)
{
	dst_block = (uint16_t)stacks;
	return raw(ZOS_RSVC_SYSCALL_STACKS, &dst_block);
}

static void fill_console_pattern(void)
{
	unsigned int index;

	for (index = 0u; index < sizeof(source); ++index) {
		source[index] = (uint8_t)(PATTERN_CON_BASE +
					  (index & PATTERN_MASK));
	}
}

static int ioc_pattern_ok(const uint8_t *buffer)
{
	unsigned int index;
	uint8_t seed = (uint8_t)(RSVC_OBJECT_HANDLE + 0u);

	for (index = 0u; index < RSVC_BLOCK_BYTES; ++index) {
		if (buffer[index] != (uint8_t)(PATTERN_IOC_BASE +
					       (uint8_t)((seed +
							 (uint8_t)index) &
							PATTERN_MASK))) {
			return 0;
		}
	}
	return 1;
}

static int same_bytes(const uint8_t *a, const uint8_t *b, unsigned int length)
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
	call_prepare(echo_handle, RSVC_OP_ECHO_EXCHANGE, 16u);
	call_req.frame.payload[0] = 0x11u;
	return call_run(ZOS_RSVC_SYSCALL_CALL, (void *)0, 0u, 0u);
}

static int op_echo_null8(void)
{
	call_prepare(echo_handle, RSVC_OP_ECHO_NULL, 8u);
	return call_run(ZOS_RSVC_SYSCALL_CALL, (void *)0, 0u, 0u);
}

static int op_echo_null16(void)
{
	call_prepare(echo_handle, RSVC_OP_ECHO_NULL, 16u);
	return call_run(ZOS_RSVC_SYSCALL_CALL, (void *)0, 0u, 0u);
}

static int op_echo_null32(void)
{
	call_prepare(echo_handle, RSVC_OP_ECHO_NULL, 32u);
	return call_run(ZOS_RSVC_SYSCALL_CALL, (void *)0, 0u, 0u);
}

static int con_write(unsigned int length)
{
	call_prepare(con_handle, RSVC_OP_CON_WRITE, 16u);
	call_req.frame.arg1 = (uint16_t)length;
	return call_run(ZOS_RSVC_SYSCALL_CALL, source, (uint16_t)length, 0u);
}

static int op_con1(void) { return con_write(1u); }
static int op_con16(void) { return con_write(16u); }
static int op_con64(void) { return con_write(64u); }
static int op_con256(void) { return con_write(256u); }

static int op_console1(void) { return zos_console_write((char *)source, 1u); }
static int op_console16(void) { return zos_console_write((char *)source, 16u); }
static int op_console64(void) { return zos_console_write((char *)source, 64u); }
static int op_console256(void) { return zos_console_write((char *)source, 256u); }

static int vfs_read(uint8_t *buffer, uint8_t flags, unsigned char number)
{
	call_prepare(vfs_handle, RSVC_OP_VFS_READ, 16u);
	call_req.frame.flags = flags;
	call_req.frame.arg0 = RSVC_OBJECT_HANDLE;
	call_req.frame.arg1 = 0u;
	return call_run(number, buffer, RSVC_BLOCK_BYTES, 1u);
}

static int op_vfs_direct(void)
{
	return vfs_read(target, 0u, ZOS_RSVC_SYSCALL_CALL);
}

static int op_vfs_staged(void)
{
	return vfs_read(target, RSVC_FRAME_STAGED, ZOS_RSVC_SYSCALL_CALL);
}

static int op_vfs_full(void)
{
	return vfs_read(target, 0u, ZOS_RSVC_SYSCALL_CALL_FULL);
}

static int op_vfs_fast(void)
{
	return vfs_read(target, 0u, ZOS_RSVC_SYSCALL_CALL);
}

/* Today's direct filesystem path, driven through the raw register ABI so
 * the comparison does not link the whole SDK filesystem wrapper set. */
static struct zfs_open_args fs_open;
static struct zfs_io_args fs_read;
static struct zfs_seek_args fs_seek;

static int op_fs_read(void)
{
	/* ZEX images carry no initialised-data areas, so these statics live in
	 * BSS and are primed on first use. */
	static int fd;
	static uint8_t fd_valid;
	int rc;

	if (!fd_valid) {
		fs_open.path = (uint16_t)fs_path;
		fs_open.length = (uint16_t)(sizeof(fs_path) - 1u);
		fs_open.flags = ZFS_O_READ;
		rc = raw(ZOS_SYSCALL_FS_OPEN, &fs_open);
		if (rc < 0) {
			return rc;
		}
		fd = rc;
		fd_valid = 1u;
	}
	/* Every measured iteration reads the same 512-byte chunk, so the
	 * baseline carries the rewind the resident source does not need. */
	fs_seek.fd = (uint8_t)fd;
	fs_seek.whence = ZFS_SEEK_SET;
	fs_seek.offset = 0;
	rc = raw(ZOS_SYSCALL_FS_SEEK, &fs_seek);
	if (rc < 0) {
		return rc;
	}
	fs_read.fd = (uint8_t)fd;
	fs_read.buffer = (uint16_t)target;
	fs_read.length = RSVC_BLOCK_BYTES;
	return raw(ZOS_SYSCALL_FS_READ, &fs_read);
}

static void phase_mark(const char *name, char suffix)
{
	zos_puts("rsvc: ");
	zos_puts(name);
	zos_putc(suffix);
	zos_puts("\r\n");
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

static void report_end(void)
{
	zos_puts("\r\n");
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
		++phase_count; \
	} while (0)

int main(int argc, char **argv)
{
	int rc;
	unsigned int index;

	(void)argc;
	(void)argv;
	zos_puts("rsvc spike: primary OK\r\n");

	echo_handle = (uint8_t)endpoint_lookup(RSVC_CLASS_ECHO, 0u);
	con_handle = (uint8_t)endpoint_lookup(RSVC_CLASS_CON, 0u);
	vfs_handle = (uint8_t)endpoint_lookup(RSVC_CLASS_VFS, 0u);
	check("lookup-echo", echo_handle != 0u);
	check("lookup-con", con_handle != 0u);
	check("lookup-vfs", vfs_handle != 0u);
	rc = evidence_read();
	check("evidence-base", rc == ZOS_RSVC_EVIDENCE_SIZE);
	check("endpoints", ev.endpoints == 6u);

	fill_console_pattern();

	/* C3: the null call and the frame-size series. */
	MEASURE("null16", MEASURE_N1, MEASURE_N2, rc = op_echo_exchange());
	check("null-status", rc == 0x12);
	check("null-echo", call_req.frame.payload[1] == 0x11u);
	MEASURE("null8", MEASURE_N1, MEASURE_N2, rc = op_echo_null8());
	check("null8-status", rc == RSVC_FRAME_MAX);
	MEASURE("null32", MEASURE_N1, MEASURE_N2, rc = op_echo_null32());
	check("null32-status", rc == RSVC_FRAME_MAX);

	/* C1/C3: con.WRITE at four sizes, then today's direct console path. */
	MEASURE("con1", MEASURE_N1, MEASURE_N2, rc = op_con1());
	check("con1-status", rc == 1);
	MEASURE("con16", MEASURE_N1, MEASURE_N2, rc = op_con16());
	check("con16-status", rc == 16);
	MEASURE("con64", MEASURE_N1, MEASURE_N2, rc = op_con64());
	check("con64-status", rc == 64);
	MEASURE("con256", MEASURE_N1, MEASURE_N2, rc = op_con256());
	check("con256-status", rc == 256);
	MEASURE("console1", MEASURE_N1, MEASURE_N2, rc = op_console1());
	check("console1-status", rc == 1);
	MEASURE("console256", MEASURE_N1, MEASURE_N2, rc = op_console256());
	check("console256-status", rc == 256);

	/* C1/C3/C5: the 512-byte chained read, direct and staged, then the
	 * same client code through the full and fast nucleus paths. */
	MEASURE("read", MEASURE_N1, MEASURE_N2, rc = op_vfs_direct());
	check("read-status", rc == RSVC_BLOCK_BYTES);
	check("read-pattern", ioc_pattern_ok(target));
	MEASURE("staged", MEASURE_N1, MEASURE_N2, rc = op_vfs_staged());
	check("staged-status", rc == RSVC_BLOCK_BYTES);
	check("staged-pattern", ioc_pattern_ok(target));
	/* C5: the same client code through the full nucleus path and the
	 * same-domain fast path; the fast run must produce the same bytes. */
	MEASURE("fastpath", MEASURE_N1, MEASURE_N2, rc = op_vfs_fast());
	check("fastpath-status", rc == RSVC_BLOCK_BYTES);
	for (index = 0u; index < RSVC_BLOCK_BYTES; ++index) {
		source[index] = target[index];
	}
	MEASURE("fullpath", MEASURE_N1, MEASURE_N2, rc = op_vfs_full());
	check("fullpath-status", rc == RSVC_BLOCK_BYTES);
	check("conform-payload", same_bytes(target, source,
					     RSVC_BLOCK_BYTES));

	/* Today's direct filesystem path on the same 512-byte chunk. */
	MEASURE("fsread", MEASURE_N1, MEASURE_N2, rc = op_fs_read());
	check("fsread-status", rc > 0);

	/* C7: re-entrancy is refused unless the endpoint declares it. */
	call_prepare(echo_handle, RSVC_OP_ECHO_SELF, 16u);
	rc = call_run(ZOS_RSVC_SYSCALL_CALL, (void *)0, 0u, 0u);
	check("reent-refused", rc == -ZOS_EBUSY);
	call_prepare(vfs_handle, RSVC_OP_VFS_SELF, 16u);
	call_req.frame.arg0 = 3u;
	rc = call_run(ZOS_RSVC_SYSCALL_CALL, (void *)0, 0u, 0u);
	check("reent-allowed", rc == 0);
	call_prepare(vfs_handle, RSVC_OP_VFS_SELF, 16u);
	call_req.frame.arg0 = 4u;
	rc = call_run(ZOS_RSVC_SYSCALL_CALL, (void *)0, 0u, 0u);
	check("depth-refused", rc == -ZOS_ENOSPC);

	/* C2: a service cannot name a caller address; forged descriptor ids
	 * and out-of-window buffers are refused. */
	call_prepare(con_handle, RSVC_OP_CON_WRITE, 16u);
	call_req.frame.arg1 = 4u;
	rc = call_run(ZOS_RSVC_SYSCALL_CALL, (void *)0x8000u, 4u, 0u);
	check("capture-refused", rc == -ZOS_EINVAL);
	call_prepare(con_handle, RSVC_OP_CON_PROBE, 16u);
	call_req.frame.payload[0] = 0x33u;
	call_req.frame.payload[1] = 0x00u;
	rc = call_run(ZOS_RSVC_SYSCALL_CALL, (void *)0, 0u, 0u);
	check("forged-refused", rc == -ZOS_EINVAL);

	/* C8: nucleus -> service upcalls, no pathname in the nucleus. */
	up_req.kind = RSVC_UPCALL_EXIT;
	up_req.reserved = 0u;
	up_req.argument = 42u;
	up_req.result = 0;
	rc = raw(ZOS_RSVC_SYSCALL_UPCALL, &up_req);
	check("upcall-exit", rc == 42);
	up_req.kind = RSVC_UPCALL_EXEC;
	up_req.argument = RSVC_OBJECT_HANDLE;
	up_req.result = 0;
	rc = raw(ZOS_RSVC_SYSCALL_UPCALL, &up_req);
	check("upcall-exec", rc == RSVC_STREAM_BYTES);

	/* C6: an IRQ owned by a service, timed with and without the
	 * service-domain handler. */
	irq_req.iterations = 0x0800u;
	irq_req.baseline = 1u;
	irq_req.reserved = 0u;
	rc = raw(ZOS_RSVC_SYSCALL_IRQ, &irq_req);
	check("irq-baseline", rc == 0);
	irq_req.iterations = 0x0800u;
	irq_req.baseline = 0u;
	rc = raw(ZOS_RSVC_SYSCALL_IRQ, &irq_req);
	check("irq-service", rc == 1);

	/* C4: stack high-water marks after the deepest graphs. */
	rc = stacks_read();
	check("stacks-read", rc == 6);
	rc = evidence_read();
	check("evidence-final", rc == ZOS_RSVC_EVIDENCE_SIZE);
	check("irq-count", ev.irq_count == 2u);
	check("irq-in-domain", ev.irq_in_domain == 1u);
	check("nested-max", ev.nested_max == 4u);
	check("reent-refusals", ev.reentrant_refused == 1u);
	check("depth-refusals", ev.depth_refused == 1u);
	check("forged", ev.desc_forged >= 1u);
	check("capture-refusals", ev.desc_refused >= 1u);
	check("fastpath", ev.fast_path_calls >= MEASURE_N2);
	check("service-calls", ev.service_calls > 0u);
	check("chain-words", ev.chain_words > 0u);
	check("exit-notify", ev.exit_notify == 1u);
	check("stream-bytes", ev.stream_bytes == RSVC_STREAM_BYTES);
	check("con-bytes", ev.con_bytes >= 256u + 16u + 64u + 1u);
	check("stacks-nonzero", stacks[0] != 0u && stacks[1] != 0u);
	check("call-latch", ev.latch_ok == 1u);

	report_u16("rsvc: calls ", ev.calls);
	report_u16(" nested ", ev.nested_max);
	report_end();
	report_u16("rsvc: stacks service ", stacks[0]);
	report_u16(" gate ", stacks[1]);
	report_u16(" isr ", stacks[2]);
	report_end();
	report_u16("rsvc: irq ", ev.irq_in_domain);
	report_u16("/", ev.irq_count);
	report_u16(" chain ", ev.chain_words);
	report_u16(" con ", ev.con_bytes);
	report_end();
	report_u16("rsvc: relay ", ev.relay_bytes);
	report_u16(" gate-sp ", ev.gate_sp_min);
	report_end();
	report_u16("rsvc: fast ", ev.fast_path_calls);
	report_u16(" refused ", ev.reentrant_refused);
	report_u16("/", ev.depth_refused);
	report_u16(" forged ", ev.desc_forged);
	report_u16(" capfail ", ev.desc_refused);
	report_end();
	report_u16("rsvc: ", checks - failures);
	report_u16("/", checks);
	zos_puts(" checks\r\n");
	if (failures == 0u) {
		zos_puts("rsvc: PASS\r\n");
	} else {
		zos_puts("rsvc: FAIL\r\n");
	}
	return 0;
}
