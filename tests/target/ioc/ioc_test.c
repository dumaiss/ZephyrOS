/* ZephyrOS IOC acceptance selftest (test artifact).
 *
 * Runs from the late boot hook after the production-style boot: the SIO
 * fallback and the IOC command+bulk transport are loaded from the ROMFS
 * recovery store, the console is bound, and `ioc.0` is registered only
 * because its CMD_LINK_SYNC bring-up produced a CRC-verified reply.
 *
 * The test reports through the direct early SIO (independent of the
 * registry and the console service) and exercises the registered ioc.0
 * device:
 *
 *   - repeated PING exchanges with an echo pattern, the controller
 *     level, and the semantic HID status;
 *   - the complete READY/DONE bulk lifecycle over the production
 *     /SHARED service: create or truncate the dedicated file
 *     IOCBULKT.BIN, write deterministic 1/128/512-byte sections (each
 *     confirmed by its matching CMD_XFER_STATUS DONE record), close,
 *     reopen for read, read all three sections back through the bulk
 *     lane, verify the patterns, and delete the file.  No diagnostic
 *     commands, raw LBAs, or user files are touched.
 *
 * If the deployed firmware answers the writable /SHARED open with the
 * unknown-command class (RSP_UNKNOWN_COMMAND plus the unknown-command
 * status) the bulk-write criterion is reported PENDING: the run ends
 * with the explicit "selftest: PENDING" marker and never the OK marker,
 * and never a destructive substitute.  A real filesystem/media failure
 * status is a failure, not PENDING.  The historical transport
 * banner (firmware level 23 / host level 07h) must not be copied: the
 * pinned deployment answers 74 (4Ah) and this host implements contract
 * level 08h.
 */

#include "zephyros/device.h"
#include "zephyros/ioc.h"
#include "zephyros/kernel.h"

#include "ioc_proto.h"
#include "ioc_selftest.h"

#if ZOS_IOC_SELFTEST

/* Frozen /SHARED file-service layouts, mirrored from the pinned MCU
 * ioc_frame.h for this fixture only (transport acceptance, not a
 * ZephyrOS filesystem API).  The controller confines every name to
 * /SHARED/ and every chunk to 512 bytes. */
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
/* Frozen controller status proving the file is absent after the cleanup
 * delete (mirrors IOC_STATUS_FS_NO_FILE in the pinned ioc_frame.h).
 * Only this status, with the empty failure shape, proves deletion: a
 * media/mount/filesystem error proves nothing. */
#define IOC_STATUS_FS_NO_FILE 0x34u
#define IOC_FS_MODE_READ  0x00u
#define IOC_FS_MODE_WRITE 0x01u
#define IOC_NAME_LEN      11u

uint8_t ace_ioc_selftest_result;

static const char ioc_digits[] = "0123456789ABCDEF";

#define IOC_PINGS 4u
#define IOC_STATUS_SAMPLES 8u

/* Dedicated acceptance file: separator-free basename, packed 8.3, and
 * never a user file.  The controller builds /SHARED/IOCBULKT.BIN from it. */
static const uint8_t ioc_name[IOC_NAME_LEN] = {
	'I', 'O', 'C', 'B', 'U', 'L', 'K', 'T', 'B', 'I', 'N'
};

/* One shared 512-byte section buffer: written from, read back into. */
static uint8_t ioc_buf[512];

/* Deterministic section pattern, identical on every run. */
static uint8_t ioc_pat(uint16_t index)
{
	return (uint8_t)((uint16_t)(index * 31u) ^ 0x5Au);
}

static void ioc_fill(uint16_t length)
{
	uint16_t i;

	for (i = 0u; i < length; ++i) {
		ioc_buf[i] = ioc_pat(i);
	}
}

static uint8_t ioc_check(uint16_t length)
{
	uint16_t i;

	for (i = 0u; i < length; ++i) {
		if (ioc_buf[i] != ioc_pat(i)) {
			return 0u;
		}
	}
	return 1u;
}

static void ioc_put_hex8(uint8_t value)
{
	early_console_putc(ioc_digits[(value >> 4) & 0x0Fu]);
	early_console_putc(ioc_digits[value & 0x0Fu]);
}

static void ioc_fail(uint8_t code, uint8_t detail)
{
	static const char prefix[] = "ioc selftest: FAIL 0x";
	static const char detail_prefix[] = " detail 0x";

	ace_ioc_selftest_result = code;
	early_console_puts(prefix);
	ioc_put_hex8(code);
	early_console_puts(detail_prefix);
	ioc_put_hex8(detail);
	early_console_puts("\r\n");
}

/* One /SHARED write chunk: FS_WRITE -> READY -> bulk_write -> DONE.
 * Returns a ZOS_IOCSELFTEST_* code. */
static uint8_t ioc_write_chunk(const struct zioc_ops *ops, void *state,
				 uint8_t handle, uint32_t offset,
				 uint16_t length)
{
	struct zioc_frame request;
	struct zioc_frame reply;
	int result;

	request.type = IOC_CMD_FS_WRITE;
	request.status = 0u;
	request.length = 7u;
	request.data[0] = handle;
	request.data[1] = (uint8_t)offset;
	request.data[2] = (uint8_t)(offset >> 8);
	request.data[3] = (uint8_t)(offset >> 16);
	request.data[4] = (uint8_t)(offset >> 24);
	request.data[5] = (uint8_t)length;
	request.data[6] = (uint8_t)(length >> 8);

	result = ops->command(state, &request, &reply);
	if (result != ZIOC_OK || reply.type != IOC_RSP_FS_WRITE ||
	    reply.status != 0u) {
		return ZOS_IOCSELFTEST_FAIL_BULK_WRITE;
	}

	result = ops->bulk_write(state, ioc_buf, length);
	if (result != ZIOC_OK) {
		return ZOS_IOCSELFTEST_FAIL_BULK_WRITE;
	}

	/* The write is complete only when the matching DONE record reports
	 * the same transfer id and success; the driver enforces the
	 * identity match, this confirms the controller status. */
	request.type = IOC_CMD_XFER_STATUS;
	request.status = 0u;
	request.length = 0u;
	result = ops->command(state, &request, &reply);
	if (result != ZIOC_OK || reply.type != IOC_RSP_XFER_STATUS ||
	    reply.length < IOC_DONE_MIN_DATA ||
	    reply.data[IOC_OFF_DONE_STATUS] != 0u) {
		return ZOS_IOCSELFTEST_FAIL_BULK_DONE;
	}
	return ZOS_IOCSELFTEST_OK;
}

/* One /SHARED read chunk: FS_READ -> READY -> bulk_read -> verify.
 * Returns a ZOS_IOCSELFTEST_* code. */
static uint8_t ioc_read_chunk(const struct zioc_ops *ops, void *state,
				uint8_t handle, uint32_t offset,
				uint16_t length)
{
	struct zioc_frame request;
	struct zioc_frame reply;
	uint16_t ready_len;
	int result;

	request.type = IOC_CMD_FS_READ;
	request.status = 0u;
	request.length = 7u;
	request.data[0] = handle;
	request.data[1] = (uint8_t)offset;
	request.data[2] = (uint8_t)(offset >> 8);
	request.data[3] = (uint8_t)(offset >> 16);
	request.data[4] = (uint8_t)(offset >> 24);
	request.data[5] = (uint8_t)length;
	request.data[6] = (uint8_t)(length >> 8);

	result = ops->command(state, &request, &reply);
	if (result != ZIOC_OK || reply.type != IOC_RSP_FS_READ ||
	    reply.status != 0u) {
		return ZOS_IOCSELFTEST_FAIL_BULK_READ;
	}
	ready_len = (uint16_t)reply.data[IOC_OFF_READY_LEN_LO] |
		    ((uint16_t)reply.data[IOC_OFF_READY_LEN_HI] << 8);
	if (ready_len != length) {
		return ZOS_IOCSELFTEST_FAIL_BULK_READ;
	}
	result = ops->bulk_read(state, ioc_buf, length);
	if (result != ZIOC_OK) {
		return ZOS_IOCSELFTEST_FAIL_BULK_READ;
	}
	if (!ioc_check(length)) {
		return ZOS_IOCSELFTEST_FAIL_BULK_DATA;
	}
	return ZOS_IOCSELFTEST_OK;
}

void zos_run_ioc_selftest(void)
{
	const struct zioc_ops *ops;
	const void *class_ops;
	void *state;
	zdev_handle_t handle;
	struct zioc_frame request;
	struct zioc_frame reply;
	uint8_t level = 0u;
	uint8_t pass;
	uint8_t i;
	uint8_t fs_handle;
	int result;
	uint16_t bits;

	ace_ioc_selftest_result = ZOS_IOCSELFTEST_FAIL_BOOT;

	/* The implemented host contract is the pinned deployed host level:
	 * this is an identifier the diagnostic prints and the smoke tool
	 * checks - not a firmware request. */
	early_console_puts("ioc: transport level ");
	ioc_put_hex8(ZIOC_TRANSPORT_LEVEL);
	early_console_puts("\r\n");

	/* Discovery is by class plus capabilities; names are diagnostic
	 * only.  The bulk package adds read and write: all four IOC
	 * capabilities must be present and every bulk pointer non-null. */
	handle = device_find(ZDEV_IOC,
			     ZCAP_IOC_COMMAND | ZCAP_IOC_BULK_READ |
				     ZCAP_IOC_BULK_WRITE | ZCAP_IOC_STATUS,
			     0u);
	if (handle == ZDEV_HANDLE_INVALID) {
		ioc_fail(ZOS_IOCSELFTEST_FAIL_BOOT, 0x01u);
		return;
	}
	if (device_interface(handle, ZDEV_IOC, &class_ops, &state) !=
	    ZDEV_OK) {
		ioc_fail(ZOS_IOCSELFTEST_FAIL_BOOT, 0x02u);
		return;
	}
	ops = (const struct zioc_ops *)class_ops;
	if (ops == (const struct zioc_ops *)0 || ops->command == 0 ||
	    ops->status == 0 || ops->bulk_read == 0 ||
	    ops->bulk_write == 0) {
		ioc_fail(ZOS_IOCSELFTEST_FAIL_BOOT, 0x03u);
		return;
	}

	/* Repeated PING: identical echo pattern every time, and the
	 * persistent link must survive without re-initialisation. */
	for (pass = 0; pass < IOC_PINGS; ++pass) {
		request.type = IOC_CMD_PING;
		request.status = 0u;
		request.length = IOC_PING_ECHO_BYTES;
		for (i = 0; i < IOC_PING_ECHO_BYTES; ++i) {
			request.data[i] = (uint8_t)(i ^ 0x5Au);
		}

		result = ops->command(state, &request, &reply);
		if (result != ZIOC_OK) {
			ioc_fail(ZOS_IOCSELFTEST_FAIL_PING, (uint8_t)(0x10u + pass));
			return;
		}
		if (reply.type != IOC_RSP_PING ||
		    reply.length != IOC_PING_DATA_LEN) {
			ioc_fail(ZOS_IOCSELFTEST_FAIL_PING,
				   (uint8_t)(0x20u + pass));
			return;
		}
		if (reply.status != 0u) {
			ioc_fail(ZOS_IOCSELFTEST_FAIL_PING,
				   (uint8_t)(0x30u + pass));
			return;
		}
		for (i = 0; i < IOC_PING_ECHO_BYTES; ++i) {
			if (reply.data[i] != request.data[i]) {
				ioc_fail(ZOS_IOCSELFTEST_FAIL_ECHO, i);
				return;
			}
		}

		/* Controller firmware level: printed once, then validated
		 * on every reply. */
		if (pass == 0u) {
			level = reply.data[IOC_PING_DATA_LEVEL];
			early_console_puts("ioc: controller level ");
			ioc_put_hex8(level);
			early_console_puts("\r\n");
		}
		if (reply.data[IOC_PING_DATA_LEVEL] != level) {
			ioc_fail(ZOS_IOCSELFTEST_FAIL_REPEAT, pass);
			return;
		}

		/* The MCU must already report the command lane established:
		 * this PING is itself a CRC-verified exchange after the
		 * LINK_SYNC bring-up. */
		if ((reply.data[IOC_PING_DATA_POWER] &
		     IOC_PING_LINK_SYNCED) == 0u) {
			ioc_fail(ZOS_IOCSELFTEST_FAIL_PING,
				   (uint8_t)(0x40u + pass));
			return;
		}
	}

	if (level != IOC_FW_LEVEL_PINNED) {
		ioc_fail(ZOS_IOCSELFTEST_FAIL_LEVEL, level);
		return;
	}
	early_console_puts("ioc: ping x4 OK\r\n");

	/* Bulk acceptance over the production /SHARED service: create or
	 * truncate the dedicated IOCBULKT.BIN, write the deterministic
	 * 1/128/512-byte sections (each confirmed by its matching DONE
	 * record), close, reopen for read, read all three back through the
	 * bulk lane, verify the patterns, and delete the file.  Never a raw
	 * LBA, a diagnostic command, or a user file. */
	request.type = IOC_CMD_FS_OPEN;
	request.status = 0u;
	request.length = 1u + IOC_NAME_LEN;
	request.data[0] = IOC_FS_MODE_WRITE;
	for (i = 0u; i < IOC_NAME_LEN; ++i) {
		request.data[1u + i] = ioc_name[i];
	}
	result = ops->command(state, &request, &reply);
	if (result == ZIOC_OK && reply.type == IOC_RSP_UNKNOWN_CMD) {
		if (reply.status != IOC_STATUS_UNKNOWN_CMD) {
			/* A nonstandard unknown-class reply proves nothing
			 * about the writable service. */
			ioc_fail(ZOS_IOCSELFTEST_FAIL_BULK_OPEN, reply.status);
			return;
		}
		/* The deployed firmware answers the writable /SHARED open
		 * with the unknown-command class: it has no writable
		 * service.  The bulk-write criterion is PENDING - an honest
		 * operator-visible result, never acceptance evidence - and
		 * the run ends without the OK marker.  Nothing was created,
		 * so nothing has to be undone. */
		early_console_puts("ioc: bulk write PENDING 0x");
		ioc_put_hex8(reply.type);
		early_console_puts("\r\n");
		ace_ioc_selftest_result = ZOS_IOCSELFTEST_PENDING;
		early_console_puts("ioc selftest: PENDING 0x");
		ioc_put_hex8(reply.type);
		early_console_puts("\r\n");
		return;
	}
	if (result != ZIOC_OK || reply.type != IOC_RSP_FS_OPEN) {
		ioc_fail(ZOS_IOCSELFTEST_FAIL_BULK_OPEN, 0x01u);
		return;
	}
	if (reply.status != 0u) {
		/* A real filesystem/media failure, not an unsupported
		 * feature: the frozen wire status does not prove the service
		 * is absent.  Report it as a failure. */
		ioc_fail(ZOS_IOCSELFTEST_FAIL_BULK_OPEN, reply.status);
		return;
	}
	{
		fs_handle = reply.data[0];
		if (fs_handle == 0u) {
			ioc_fail(ZOS_IOCSELFTEST_FAIL_BULK_OPEN, 0x02u);
			return;
		}

		ioc_fill(1u);
		result = ioc_write_chunk(ops, state, fs_handle, 0u, 1u);
		if (result != ZIOC_OK) {
			ioc_fail((uint8_t)result, 0x11u);
			return;
		}
		ioc_fill(128u);
		result = ioc_write_chunk(ops, state, fs_handle, 1u, 128u);
		if (result != ZIOC_OK) {
			ioc_fail((uint8_t)result, 0x12u);
			return;
		}
		ioc_fill(512u);
		result = ioc_write_chunk(ops, state, fs_handle, 129u, 512u);
		if (result != ZIOC_OK) {
			ioc_fail((uint8_t)result, 0x13u);
			return;
		}

		/* Exact final size: 1 + 128 + 512 bytes. */
		request.type = IOC_CMD_FS_CLOSE;
		request.status = 0u;
		request.length = 5u;
		request.data[0] = fs_handle;
		request.data[1] = 0x81u;
		request.data[2] = 0x02u;
		request.data[3] = 0x00u;
		request.data[4] = 0x00u;
		result = ops->command(state, &request, &reply);
		if (result != ZIOC_OK || reply.type != IOC_RSP_FS_CLOSE ||
		    reply.status != 0u) {
			ioc_fail(ZOS_IOCSELFTEST_FAIL_BULK_CLOSE, 0x14u);
			return;
		}
		early_console_puts("ioc: bulk write 1/128/512 OK\r\n");
		early_console_puts("ioc: done x3 OK\r\n");

		/* Reopen for read and verify all three sections. */
		request.type = IOC_CMD_FS_OPEN;
		request.status = 0u;
		request.length = 1u + IOC_NAME_LEN;
		request.data[0] = IOC_FS_MODE_READ;
		for (i = 0u; i < IOC_NAME_LEN; ++i) {
			request.data[1u + i] = ioc_name[i];
		}
		result = ops->command(state, &request, &reply);
		if (result != ZIOC_OK || reply.type != IOC_RSP_FS_OPEN ||
		    reply.status != 0u || reply.data[0] == 0u) {
			ioc_fail(ZOS_IOCSELFTEST_FAIL_BULK_OPEN, 0x03u);
			return;
		}
		fs_handle = reply.data[0];

		result = ioc_read_chunk(ops, state, fs_handle, 0u, 1u);
		if (result != ZIOC_OK) {
			ioc_fail((uint8_t)result, 0x21u);
			return;
		}
		result = ioc_read_chunk(ops, state, fs_handle, 1u, 128u);
		if (result != ZIOC_OK) {
			ioc_fail((uint8_t)result, 0x22u);
			return;
		}
		result = ioc_read_chunk(ops, state, fs_handle, 129u, 512u);
		if (result != ZIOC_OK) {
			ioc_fail((uint8_t)result, 0x23u);
			return;
		}

		request.type = IOC_CMD_FS_CLOSE;
		request.status = 0u;
		request.length = 5u;
		request.data[0] = fs_handle;
		request.data[1] = 0x81u;
		request.data[2] = 0x02u;
		request.data[3] = 0x00u;
		request.data[4] = 0x00u;
		result = ops->command(state, &request, &reply);
		if (result != ZIOC_OK || reply.type != IOC_RSP_FS_CLOSE ||
		    reply.status != 0u) {
			ioc_fail(ZOS_IOCSELFTEST_FAIL_BULK_CLOSE, 0x15u);
			return;
		}

		request.type = IOC_CMD_FS_DELETE;
		request.status = 0u;
		request.length = IOC_NAME_LEN;
		for (i = 0u; i < IOC_NAME_LEN; ++i) {
			request.data[i] = ioc_name[i];
		}
		result = ops->command(state, &request, &reply);
		if (result != ZIOC_OK || reply.type != IOC_RSP_FS_DELETE ||
		    reply.status != 0u) {
			ioc_fail(ZOS_IOCSELFTEST_FAIL_BULK_DELETE, 0x16u);
			return;
		}
		early_console_puts("ioc: bulk read 1/128/512 OK\r\n");

		/* Cleanup proof: the dedicated file must no longer open.
		 * An open that still succeeds would mean the delete failed
		 * or targeted the wrong name. */
		request.type = IOC_CMD_FS_OPEN;
		request.status = 0u;
		request.length = 1u + IOC_NAME_LEN;
		request.data[0] = IOC_FS_MODE_READ;
		for (i = 0u; i < IOC_NAME_LEN; ++i) {
			request.data[1u + i] = ioc_name[i];
		}
		result = ops->command(state, &request, &reply);
		/* Only the frozen no-file failure with its empty payload shape
		 * proves the file is gone: a media/mount/filesystem error, or a
		 * failure reply that still carries data, is NOT cleanup
		 * evidence. */
		if (result != ZIOC_OK || reply.type != IOC_RSP_FS_OPEN ||
		    reply.status != IOC_STATUS_FS_NO_FILE ||
		    reply.length != 0u) {
			ioc_fail(ZOS_IOCSELFTEST_FAIL_BULK_DELETE, 0x17u);
			return;
		}
		early_console_puts("ioc: cleanup OK 0x");
		ioc_put_hex8(reply.status);
		early_console_puts("\r\n");
	}

	/* Semantic status only: the returned mask may carry nothing but
	 * ZIOC_STATUS_HID_PENDING (and the doorbell varies with whatever
	 * the controller has queued, so both values are legal). */
	for (i = 0; i < IOC_STATUS_SAMPLES; ++i) {
		bits = ops->status(state);
		if ((bits & (uint16_t)~ZIOC_STATUS_HID_PENDING) != 0u) {
			ioc_fail(ZOS_IOCSELFTEST_FAIL_STATUS, (uint8_t)bits);
			return;
		}
	}
	early_console_puts("ioc: hid status ");
	ioc_put_hex8((uint8_t)(bits & 0xFFu));
	early_console_puts("\r\n");

	ace_ioc_selftest_result = ZOS_IOCSELFTEST_OK;
	early_console_puts("ioc selftest: OK\r\n");
}

#endif /* ZOS_IOC_SELFTEST */
