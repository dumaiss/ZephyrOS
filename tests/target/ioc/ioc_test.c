/* ZephyrOS IOC resident-service acceptance diagnostic (work package 41).
 *
 * Runs from the late boot hook, after the service loader brought the
 * image's service set (sio, ioc, console) up in bank 6 and after the
 * nucleus console probe: the IOC service's init has run the bounded
 * LINK_SYNC bring-up and its endpoint (class ZOS_CLASS_IOC, instance 0,
 * protocol version ZOS_PROTOCOL_VERSION) answers.
 *
 * The diagnostic is a *bank-7 client* of that service, exactly like
 * the retired class shim: it holds no buffer descriptor and cannot use
 * `move`, so it drives the IOC protocol (abi/zos/protocol.h) through the
 * descriptor-less operations and the staged buffers the replies name, with
 * the shared acceptance client (tests/target/support/service_client.c) for
 * the calls, the copy-gate moves and the early-SIO transcript.  It sees
 * native `struct zioc_frame` records; framing, CRC, sequence and lane setup
 * never surface.
 *
 * Phases: endpoint discovery (class 05, version 01); ZIOC_OP_READY = 1; one
 * ZIOC_OP_COMMAND (CMD_PING) whose 29-byte staged reply frame is read back
 * and checked for type, echo pattern, controller level 4Ah and the
 * link-synced snapshot; ZIOC_OP_STATUS with no bit outside
 * ZIOC_STATUS_HID_PENDING; the staged bulk lane over the frozen /SHARED
 * file service (FS_WRITE arms a Z80->MCU phase, the pattern is placed at
 * the published bulk address and committed descriptor-less with
 * ZIOC_OP_BULK_WRITE, the DONE record confirms it, FS_READ arms the reverse
 * direction, ZIOC_OP_BULK_READ completes it, the staged bytes are read back
 * and compared, then the fixture file is closed and deleted); and malformed
 * calls refused with -ZOS_EINVAL (unknown operation, oversize request DATA,
 * descriptor-less REPLY, handle 0 and a bogus handle).
 *
 * If the deployed firmware answers the writable /SHARED open with the
 * unknown-command class it has no writable service: the bulk criterion is
 * PENDING - never the OK marker, never acceptance evidence.  The last line
 * is "ioc selftest: OK" or "ioc selftest: FAIL <phase>", with the matching
 * ZOS_IOCSELFTEST_* code in ace_ioc_selftest_result.
 */

#include "zephyros/errno.h"
#include "zephyros/ioc.h"
#include "zos/protocol.h"

#include "ioc_proto.h"
#include "ioc_selftest.h"
#include "service_client.h"

#if ZOS_IOC_SELFTEST

/* Frozen /SHARED file-service classes (mirrored from the pinned MCU
 * ioc_frame.h for this fixture only, as the M8 diagnostic did) and the
 * dedicated acceptance file the controller builds from the packed 8.3
 * basename.  FS_WRITE/FS_READ are also two of the request classes the
 * transport admits a bulk phase for. */
#define IOC_CMD_FS_OPEN   0x22u
#define IOC_RSP_FS_OPEN   0xA2u
#define IOC_CMD_FS_READ   0x23u
#define IOC_RSP_FS_READ   0xA3u
#define IOC_CMD_FS_WRITE  0x24u
#define IOC_RSP_FS_WRITE  0xA4u
#define IOC_CMD_FS_CLOSE  0x25u
#define IOC_RSP_FS_CLOSE  0xA5u
#define IOC_CMD_FS_DELETE 0x27u
#define IOC_RSP_FS_DELETE 0xA7u
#define IOC_FS_MODE_WRITE 0x01u
#define IOC_NAME_LEN      11u
#define IOC_BULK_BYTES    256u /* the one staged bulk section */

uint8_t ace_ioc_selftest_result;

static const uint8_t ioc_name[IOC_NAME_LEN] = {
	'I', 'O', 'C', 'B', 'U', 'L', 'K', 'T', 'B', 'I', 'N'
};
static uint8_t ioc_pattern[IOC_BULK_BYTES];
static struct zioc_frame ioc_reply;
static struct zos_ipc_frame ioc_frame;
static uint8_t ioc_file;
static uint8_t ioc_xfer_id;

static void ioc_fail(const char *phase, uint8_t code)
{
	ace_ioc_selftest_result = code;
	service_cli_puts("ioc selftest: FAIL ");
	service_cli_puts(phase);
	service_cli_puts("\r\n");
}

/* Transcript line "<label><hex>\r\n"; hex only, because the nucleus links
 * no SDCC division helpers (service_client.h). */
static void ioc_show8(const char *label, uint8_t value)
{
	service_cli_puts(label);
	service_cli_hex8(value);
	service_cli_puts("\r\n");
}

static void ioc_show16(const char *label, uint16_t value)
{
	service_cli_puts(label);
	service_cli_hex16(value);
	service_cli_puts("\r\n");
}

/* The deterministic byte the PING echo and the bulk section carry. */
static uint8_t ioc_pattern_byte(uint16_t index)
{
	return (uint8_t)(index ^ 0x5Au);
}

static void ioc_fill_pattern(void)
{
	uint16_t index;

	for (index = 0u; index < IOC_BULK_BYTES; ++index) {
		ioc_pattern[index] = ioc_pattern_byte(index);
	}
}

static uint8_t ioc_pattern_ok(void)
{
	uint16_t index;

	for (index = 0u; index < IOC_BULK_BYTES; ++index) {
		if (ioc_pattern[index] != ioc_pattern_byte(index)) {
			return 0u;
		}
	}
	return 1u;
}

/* Frame one descriptor-less ZIOC_OP_COMMAND (the payload is zeroed; the
 * caller fills the request DATA) and clear the staged reply. */
static void ioc_prepare(uint8_t type, uint8_t length)
{
	service_cli_frame(&ioc_frame, ZIOC_OP_COMMAND, type, length);
	ioc_reply.type = 0u;
	ioc_reply.status = 0u;
	ioc_reply.length = 0u;
}

/* Issue the framed command, then stage-read the 29-byte native reply frame
 * the reply words describe (payload[0..1], the staging handshake). */
static int16_t ioc_exchange(uint8_t handle)
{
	uint16_t address;

	if (service_cli_call(handle, &ioc_frame) != 0) {
		return -1;
	}
	address = (uint16_t)((uint16_t)ioc_frame.payload[0] |
			     ((uint16_t)ioc_frame.payload[1] << 8));
	if (address == 0u ||
	    service_cli_stage_read(address, ZIOC_FRAME_SIZE,
				   (uint8_t *)&ioc_reply) == 0) {
		return -1;
	}
	return 0;
}

/* The READY record of a bulk-arming reply: nonzero transfer id, the
 * direction the request armed, and the exact admitted length. */
static uint8_t ioc_ready_ok(uint8_t type, uint8_t direction, uint16_t length)
{
	ioc_xfer_id = ioc_reply.data[IOC_OFF_READY_XFER_ID];
	return (uint8_t)(ioc_reply.type == type && ioc_reply.status == 0u &&
			 ioc_xfer_id != 0u &&
			 ioc_reply.data[IOC_OFF_READY_DIRECTION] == direction &&
			 (uint16_t)((uint16_t)ioc_reply.data[IOC_OFF_READY_LEN_LO] |
				    ((uint16_t)ioc_reply.data[IOC_OFF_READY_LEN_HI]
				     << 8)) == length);
}

/* One CMD_PING: the staged reply frame carries the echoed pattern, the
 * controller level and the link-synced power snapshot. */
static uint8_t ioc_ping(uint8_t handle)
{
	uint8_t index;
	uint8_t echoed = 0u;

	ioc_prepare(IOC_CMD_PING, (uint8_t)IOC_PING_ECHO_BYTES);
	for (index = 0u; index < (uint8_t)IOC_PING_ECHO_BYTES; ++index) {
		ioc_frame.payload[index] = ioc_pattern[index];
	}
	if (ioc_exchange(handle) != 0 || ioc_reply.type != IOC_RSP_PING ||
	    ioc_reply.status != 0u ||
	    ioc_reply.length != (uint8_t)IOC_PING_DATA_LEN ||
	    ioc_frame.arg0 != IOC_RSP_PING ||
	    ioc_frame.arg1 != (uint16_t)IOC_PING_DATA_LEN ||
	    ioc_frame.payload[2] != 0u) {
		return 0u;
	}
	for (index = 0u; index < (uint8_t)IOC_PING_ECHO_BYTES; ++index) {
		if (ioc_reply.data[(uint8_t)(IOC_PING_DATA_ECHO + index)] ==
		    ioc_pattern[index]) {
			++echoed;
		}
	}
	if (echoed != (uint8_t)IOC_PING_ECHO_BYTES ||
	    ioc_reply.data[IOC_PING_DATA_LEVEL] != (uint8_t)IOC_FW_LEVEL_PINNED ||
	    (ioc_reply.data[IOC_PING_DATA_POWER] & IOC_PING_LINK_SYNCED) == 0u) {
		return 0u;
	}
	service_cli_puts("ioc: reply ");
	service_cli_hex8(ioc_reply.type);
	service_cli_puts(" status ");
	service_cli_hex8(ioc_reply.status);
	service_cli_puts(" length ");
	service_cli_hex8(ioc_reply.length);
	service_cli_puts("\r\n");
	service_cli_puts("ioc: echo ");
	service_cli_hex8(echoed);
	service_cli_puts(" level ");
	service_cli_hex8(ioc_reply.data[IOC_PING_DATA_LEVEL]);
	service_cli_puts(" power ");
	service_cli_hex8(ioc_reply.data[IOC_PING_DATA_POWER]);
	service_cli_puts("\r\n");
	return 1u;
}

/* The 7-byte FS transfer DATA (handle, offset 0, length) in the payload. */
static void ioc_fs_transfer(uint8_t file)
{
	ioc_frame.payload[0] = file;
	ioc_frame.payload[5] = (uint8_t)IOC_BULK_BYTES;
	ioc_frame.payload[6] = (uint8_t)(IOC_BULK_BYTES >> 8);
}

/* The staged bulk write: place the pattern at the bulk address the READY
 * reply published, commit it with the descriptor-less ZIOC_OP_BULK_WRITE,
 * and read the matching DONE record back. */
static uint8_t ioc_bulk_write(uint8_t handle)
{
	uint16_t address;

	ioc_prepare(IOC_CMD_FS_WRITE, 7u);
	ioc_fs_transfer(ioc_file);
	if (ioc_exchange(handle) != 0 ||
	    !ioc_ready_ok(IOC_RSP_FS_WRITE, IOC_BULK_DIR_Z80_TO_MCU,
			  IOC_BULK_BYTES)) {
		return 0u;
	}
	address = (uint16_t)((uint16_t)ioc_frame.payload[3] |
			     ((uint16_t)ioc_frame.payload[4] << 8));
	if (address == 0u ||
	    service_cli_stage_write(address, IOC_BULK_BYTES, ioc_pattern) == 0) {
		return 0u;
	}
	service_cli_frame(&ioc_frame, ZIOC_OP_BULK_WRITE, 0u, IOC_BULK_BYTES);
	if (service_cli_call(handle, &ioc_frame) != (int16_t)IOC_BULK_BYTES) {
		return 0u;
	}
	ioc_prepare(IOC_CMD_XFER_STATUS, 0u);
	return (uint8_t)(ioc_exchange(handle) == 0 &&
			 ioc_reply.type == IOC_RSP_XFER_STATUS &&
			 ioc_reply.status == 0u &&
			 ioc_reply.length >= (uint8_t)IOC_DONE_MIN_DATA &&
			 ioc_reply.data[IOC_OFF_DONE_XFER_ID] == ioc_xfer_id &&
			 ioc_reply.data[IOC_OFF_DONE_STATUS] == IOC_STATUS_OK);
}

/* The staged bulk read: complete the admitted MCU->Z80 phase into the
 * service buffer, then read that buffer back through the copy gate. */
static uint8_t ioc_bulk_read(uint8_t handle)
{
	uint16_t address;

	ioc_prepare(IOC_CMD_FS_READ, 7u);
	ioc_fs_transfer(ioc_file);
	if (ioc_exchange(handle) != 0 ||
	    !ioc_ready_ok(IOC_RSP_FS_READ, IOC_BULK_DIR_MCU_TO_Z80,
			  IOC_BULK_BYTES)) {
		return 0u;
	}
	service_cli_frame(&ioc_frame, ZIOC_OP_BULK_READ, 0u, IOC_BULK_BYTES);
	if (service_cli_call(handle, &ioc_frame) != (int16_t)IOC_BULK_BYTES) {
		return 0u;
	}
	address = (uint16_t)((uint16_t)ioc_frame.payload[0] |
			     ((uint16_t)ioc_frame.payload[1] << 8));
	if (address == 0u ||
	    service_cli_stage_read(address, IOC_BULK_BYTES, ioc_pattern) == 0) {
		return 0u;
	}
	return ioc_pattern_ok();
}

/* Close the fixture file at its exact final size, then delete it: the
 * acceptance leaves nothing behind. */
static uint8_t ioc_cleanup(uint8_t handle)
{
	uint8_t index;

	ioc_prepare(IOC_CMD_FS_CLOSE, 5u); /* handle, size 0x00000100 */
	ioc_frame.payload[0] = ioc_file;
	ioc_frame.payload[2] = (uint8_t)(IOC_BULK_BYTES >> 8);
	if (ioc_exchange(handle) != 0 || ioc_reply.type != IOC_RSP_FS_CLOSE ||
	    ioc_reply.status != 0u) {
		return 0u;
	}
	ioc_prepare(IOC_CMD_FS_DELETE, (uint8_t)IOC_NAME_LEN);
	for (index = 0u; index < (uint8_t)IOC_NAME_LEN; ++index) {
		ioc_frame.payload[index] = ioc_name[index];
	}
	return (uint8_t)(ioc_exchange(handle) == 0 &&
			 ioc_reply.type == IOC_RSP_FS_DELETE &&
			 ioc_reply.status == 0u);
}

void zos_run_ioc_selftest(void)
{
	/* The malformed command calls: an unknown operation, a request DATA
	 * length above the inline maximum, and the descriptor-less REPLY form
	 * (a bank-7 client reads the staged frame instead). */
	static const uint8_t bad_op[3] = {
		0x7Fu, ZIOC_OP_COMMAND, ZIOC_OP_REPLY
	};
	uint8_t handle = 0u;
	uint8_t refused = 0u;
	uint8_t index;
	int16_t result;
	uint16_t word;

	ace_ioc_selftest_result = ZOS_IOCSELFTEST_FAIL_ENDPOINT;
	ioc_fill_pattern();

	if (service_cli_endpoint(ZOS_CLASS_IOC, &handle) == 0u) {
		ioc_fail("endpoint", ZOS_IOCSELFTEST_FAIL_ENDPOINT);
		return;
	}
	service_cli_puts("ioc: endpoint class ");
	service_cli_hex8(ZOS_CLASS_IOC);
	ioc_show8(" version ", ZOS_PROTOCOL_VERSION);

	result = service_cli_getc(handle, ZIOC_OP_READY);
	if (result != 1) {
		ioc_fail("ready", ZOS_IOCSELFTEST_FAIL_READY);
		return;
	}
	ioc_show8("ioc: ready ", (uint8_t)result);

	if (!ioc_ping(handle)) {
		ioc_fail("command", ZOS_IOCSELFTEST_FAIL_COMMAND);
		return;
	}

	/* Semantic status only: the word may carry nothing but the HID
	 * doorbell bit, which varies with what the peer has queued. */
	result = service_cli_getc(handle, ZIOC_OP_STATUS);
	word = (uint16_t)result;
	if (result < 0 || (word & (uint16_t)~ZIOC_STATUS_HID_PENDING) != 0u) {
		ioc_fail("status", ZOS_IOCSELFTEST_FAIL_STATUS);
		return;
	}
	ioc_show16("ioc: status ", word);

	/* Create the dedicated fixture file. */
	ioc_prepare(IOC_CMD_FS_OPEN, (uint8_t)(1u + IOC_NAME_LEN));
	ioc_frame.payload[0] = IOC_FS_MODE_WRITE;
	for (index = 0u; index < (uint8_t)IOC_NAME_LEN; ++index) {
		ioc_frame.payload[1u + index] = ioc_name[index];
	}
	if (ioc_exchange(handle) != 0 || ioc_reply.type != IOC_RSP_FS_OPEN ||
	    ioc_reply.status != 0u || ioc_reply.length < 1u ||
	    ioc_reply.data[0] == 0u) {
		if (ioc_reply.type == IOC_RSP_UNKNOWN_CMD &&
		    ioc_reply.status == IOC_STATUS_UNKNOWN_CMD) {
			/* The deployed firmware serves no writable /SHARED
			 * file: PENDING, never the OK marker.  Nothing was
			 * created, so nothing has to be undone. */
			ioc_show8("ioc: bulk write PENDING 0x", ioc_reply.type);
			ace_ioc_selftest_result = ZOS_IOCSELFTEST_PENDING;
			ioc_show8("ioc selftest: PENDING 0x", ioc_reply.type);
			return;
		}
		ioc_fail("bulk", ZOS_IOCSELFTEST_FAIL_BULK);
		return;
	}
	ioc_file = ioc_reply.data[0];

	if (!ioc_bulk_write(handle)) {
		ioc_fail("bulk", ZOS_IOCSELFTEST_FAIL_BULK);
		return;
	}
	service_cli_puts("ioc: bulk write ");
	service_cli_hex16(IOC_BULK_BYTES);
	service_cli_puts(" OK\r\n");
	if (!ioc_bulk_read(handle)) {
		ioc_fail("bulk", ZOS_IOCSELFTEST_FAIL_BULK);
		return;
	}
	service_cli_puts("ioc: bulk read ");
	service_cli_hex16(IOC_BULK_BYTES);
	service_cli_puts(" OK\r\n");
	if (!ioc_cleanup(handle)) {
		ioc_fail("bulk", ZOS_IOCSELFTEST_FAIL_BULK);
		return;
	}
	service_cli_puts("ioc: cleanup OK\r\n");

	/* Malformed calls are refused with -ZOS_EINVAL before any wire
	 * traffic, including the invalid handles. */
	for (index = 0u; index < 3u; ++index) {
		service_cli_frame(&ioc_frame, bad_op[index], 0u,
				  (uint16_t)((index == 1u) ? 25u :
					     (index == 2u) ? 29u : 0u));
		refused = (uint8_t)(refused +
				    (service_cli_call(handle, &ioc_frame) ==
				     -ZOS_EINVAL));
	}
	service_cli_frame(&ioc_frame, ZIOC_OP_STATUS, 0u, 0u);
	refused = (uint8_t)(refused +
			    (service_cli_call(0u, &ioc_frame) == -ZOS_EINVAL));
	refused = (uint8_t)(refused +
			    (service_cli_call((uint8_t)0x7Fu, &ioc_frame) ==
			     -ZOS_EINVAL));
	if (refused != 5u) {
		ioc_fail("refused", ZOS_IOCSELFTEST_FAIL_REFUSED);
		return;
	}
	ioc_show8("ioc: malformed refused ", refused);

	ace_ioc_selftest_result = ZOS_IOCSELFTEST_OK;
	service_cli_puts("ioc selftest: OK\r\n");
}

#endif /* ZOS_IOC_SELFTEST */
