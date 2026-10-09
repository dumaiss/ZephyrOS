/* ZephyrOS IOC class for the bank-7 IOC-FAT provider (work package 41).
 *
 * Publishes the resident IOC service's transport to a module that lives in
 * the cold half's driver arena, in the shape the module already asks for:
 * the device-registry query the M8 IOC device answered is now answered
 * from the service's endpoint presence, and the class operations are this
 * file's transport (see ioc_shim.h for why and for its removal in package
 * 42).
 *
 * Transport shape (abi/zos/protocol.h, class ZOS_CLASS_IOC):
 *
 *   - control: ipc_nucleus_call() with the frame carrying the request
 *     type, the request DATA length and up to ZIOC_PROTOCOL_REQUEST_MAX
 *     DATA bytes inline.  The service answers with the reply frame's
 *     service-domain address, so the reply is read back through the copy
 *     gate;
 *   - bulk: the service names its own 512-byte buffer (the COMMAND reply
 *     reports it, the staged BULK_READ reply reports it again) and the
 *     bytes move through the common copy gate in ZOS_SYSCALL_STAGE_SIZE
 *     chunks: the service window is not mapped while a bank-7 module runs,
 *     and a bank-7 caller cannot hold an IPC descriptor.
 *
 * The gate is single-depth and its staging buffer is the common one
 * (kernel/memory/user_copy.asm), exactly as the service loader uses it to
 * place an image.  Every copy here is inside one nucleus-side operation,
 * so nothing can be nested inside it.
 */

#include "zephyros/errno.h"
#include "zephyros/ipc.h"
#include "zephyros/platform.h"
#include "zephyros/process.h"

#include "zos/protocol.h"

#include "kernel/ipc/ipc.h"
#include "kernel/syscall/diag.h"
#include "kernel/module/ioc_shim.h"

/* The resident service bank: bank 7 is the nucleus, so the last
 * application bank is the services' (architecture 36.2). */
#define IOC_SHIM_SERVICE_BANK (uint8_t)(ZOS_BANK_MASK - 1u)

/* The synthetic class handle.  It is never a registry handle, and 0 stays
 * ZDEV_HANDLE_INVALID, so a module cannot mistake one for the other. */
#define IOC_SHIM_HANDLE 1u

/* The copy gate's staging buffer: common-resident, ZOS_SYSCALL_STAGE_SIZE
 * bytes, shared with the loader and the syscall bodies (single-depth, one
 * user at a time). */
extern uint8_t zos_user_copy_staging[];

struct ioc_shim_state {
	uint8_t endpoint;      /* resident IOC endpoint handle          */
	uint16_t bulk_address; /* the service's bulk buffer (0 = unset) */
};

static struct ioc_shim_state ioc_shim;
static struct zos_ipc_frame ioc_shim_frame;

static struct ioc_shim_state *ioc_shim_for(void *state)
{
	return (state != (void *)0) ? (struct ioc_shim_state *)state : &ioc_shim;
}

/* One bounded transfer through the common copy gate. */
static int ioc_shim_gate(uint8_t op, uint16_t address, uint16_t length)
{
	struct zos_bank_copy_req req;

	req.bank = IOC_SHIM_SERVICE_BANK;
	req.op = op;
	req.addr = address;
	req.buffer = zos_user_copy_staging;
	req.length = length;
	return (zos_bank_copy(&req) == 0u) ? ZIOC_OK : ZIOC_EIO;
}

/* One command exchange: the request travels in the frame, the reply frame
 * comes back through the gate.  The service validates the wire reply
 * exactly as the M8 driver did - its CRC, sequence and class tag never
 * leave the service - so the status passes through unchanged. */
static int ioc_shim_command(void *vstate, const struct zioc_frame *request,
			    struct zioc_frame *reply)
{
	struct ioc_shim_state *state = ioc_shim_for(vstate);
	uint16_t frame_address;
	uint16_t index;
	int result;

	if (request == (const struct zioc_frame *)0 ||
	    reply == (struct zioc_frame *)0 || state->endpoint == 0u) {
		return ZIOC_EINVAL;
	}
	if (request->length > ZIOC_PROTOCOL_REQUEST_MAX) {
		return ZIOC_EINVAL;
	}
	ioc_shim_frame.op = ZIOC_OP_COMMAND;
	ioc_shim_frame.flags = 0u;
	ioc_shim_frame.desc = ZOS_IPC_DESC_NONE;
	ioc_shim_frame.arg0 = request->type;
	ioc_shim_frame.arg1 = request->length;
	for (index = 0u; index < request->length; ++index) {
		ioc_shim_frame.payload[index] = request->data[index];
	}
	result = ipc_nucleus_call(state->endpoint, &ioc_shim_frame);
	if (result != 0) {
		return result;
	}
	frame_address = (uint16_t)((uint16_t)ioc_shim_frame.payload[0] |
				   ((uint16_t)ioc_shim_frame.payload[1] << 8));
	if (ioc_shim_gate(ZOS_BANK_OP_READ, frame_address,
			  ZIOC_FRAME_SIZE) != ZIOC_OK) {
		return ZIOC_EIO;
	}
	for (index = 0u; index < ZIOC_FRAME_SIZE; ++index) {
		((uint8_t *)reply)[index] = zos_user_copy_staging[index];
	}
	state->bulk_address =
		(uint16_t)((uint16_t)ioc_shim_frame.payload[3] |
			   ((uint16_t)ioc_shim_frame.payload[4] << 8));
	return ZIOC_OK;
}

/* The staged bulk phase: the service reads the lane into its own buffer and
 * names it, this side drains that buffer into the module's. */
static int ioc_shim_bulk_read(void *vstate, void *dst, uint16_t length)
{
	struct ioc_shim_state *state = ioc_shim_for(vstate);
	uint8_t *out = (uint8_t *)dst;
	uint16_t address;
	uint16_t done = 0u;
	uint16_t index;
	int result;

	if (dst == (void *)0 || length == 0u || length > ZIOC_BULK_MAX ||
	    state->endpoint == 0u) {
		return ZIOC_EINVAL;
	}
	ioc_shim_frame.op = ZIOC_OP_BULK_READ;
	ioc_shim_frame.flags = 0u;
	ioc_shim_frame.desc = ZOS_IPC_DESC_NONE;
	ioc_shim_frame.arg0 = 0u;
	ioc_shim_frame.arg1 = length;
	result = ipc_nucleus_call(state->endpoint, &ioc_shim_frame);
	if (result != (int)length) {
		return (result < 0) ? result : ZIOC_EIO;
	}
	address = (uint16_t)((uint16_t)ioc_shim_frame.payload[0] |
			     ((uint16_t)ioc_shim_frame.payload[1] << 8));
	state->bulk_address = address;
	while (done < length) {
		uint16_t chunk = (uint16_t)(length - done);

		if (chunk > ZOS_SYSCALL_STAGE_SIZE) {
			chunk = ZOS_SYSCALL_STAGE_SIZE;
		}
		if (ioc_shim_gate(ZOS_BANK_OP_READ,
				  (uint16_t)(address + done), chunk) != ZIOC_OK) {
			return ZIOC_EIO;
		}
		for (index = 0u; index < chunk; ++index) {
			out[(uint16_t)(done + index)] =
				zos_user_copy_staging[index];
		}
		done = (uint16_t)(done + chunk);
	}
	return ZIOC_OK;
}

/* The staged bulk phase in the write direction: place the module's bytes
 * where the service said its buffer is, then let the service commit them
 * to the lane. */
static int ioc_shim_bulk_write(void *vstate, const void *src, uint16_t length)
{
	struct ioc_shim_state *state = ioc_shim_for(vstate);
	const uint8_t *in = (const uint8_t *)src;
	uint16_t done = 0u;
	uint16_t index;
	int result;

	if (src == (const void *)0 || length == 0u || length > ZIOC_BULK_MAX ||
	    state->endpoint == 0u || state->bulk_address == 0u) {
		return ZIOC_EINVAL;
	}
	while (done < length) {
		uint16_t chunk = (uint16_t)(length - done);

		if (chunk > ZOS_SYSCALL_STAGE_SIZE) {
			chunk = ZOS_SYSCALL_STAGE_SIZE;
		}
		for (index = 0u; index < chunk; ++index) {
			zos_user_copy_staging[index] =
				in[(uint16_t)(done + index)];
		}
		if (ioc_shim_gate(ZOS_BANK_OP_WRITE,
				  (uint16_t)(state->bulk_address + done),
				  chunk) != ZIOC_OK) {
			return ZIOC_EIO;
		}
		done = (uint16_t)(done + chunk);
	}
	ioc_shim_frame.op = ZIOC_OP_BULK_WRITE;
	ioc_shim_frame.flags = 0u;
	ioc_shim_frame.desc = ZOS_IPC_DESC_NONE;
	ioc_shim_frame.arg0 = 0u;
	ioc_shim_frame.arg1 = length;
	result = ipc_nucleus_call(state->endpoint, &ioc_shim_frame);
	if (result != (int)length) {
		return (result < 0) ? result : ZIOC_EIO;
	}
	return ZIOC_OK;
}

static uint16_t ioc_shim_status(void *vstate)
{
	struct ioc_shim_state *state = ioc_shim_for(vstate);
	int result;

	if (state->endpoint == 0u) {
		return 0u;
	}
	ioc_shim_frame.op = ZIOC_OP_STATUS;
	ioc_shim_frame.flags = 0u;
	ioc_shim_frame.desc = ZOS_IPC_DESC_NONE;
	ioc_shim_frame.arg0 = 0u;
	ioc_shim_frame.arg1 = 0u;
	result = ipc_nucleus_call(state->endpoint, &ioc_shim_frame);
	if (result < 0) {
		return 0u;
	}
	return (uint16_t)result;
}

static const struct zioc_ops ioc_shim_class_ops = {
	ioc_shim_command,
	ioc_shim_bulk_read,
	ioc_shim_bulk_write,
	ioc_shim_status,
};

const struct zioc_ops *ioc_shim_ops(void)
{
	return &ioc_shim_class_ops;
}

void *ioc_shim_state(void)
{
	return (void *)&ioc_shim;
}

zdev_handle_t ioc_shim_find(zdev_caps_t required_caps, zdev_handle_t after)
{
	uint8_t version = 0u;
	int8_t handle;

	if (after != ZDEV_HANDLE_INVALID) {
		return ZDEV_HANDLE_INVALID;
	}
	if ((required_caps &
	     (zdev_caps_t)~(ZCAP_IOC_COMMAND | ZCAP_IOC_BULK_READ |
			    ZCAP_IOC_BULK_WRITE | ZCAP_IOC_STATUS)) != 0u) {
		return ZDEV_HANDLE_INVALID;
	}
	handle = ipc_lookup(ZOS_CLASS_IOC, ZOS_CLASS_INSTANCE, &version);
	if (handle <= 0 || version != ZOS_PROTOCOL_VERSION) {
		return ZDEV_HANDLE_INVALID;
	}
	ioc_shim.endpoint = (uint8_t)handle;
	ioc_shim.bulk_address = 0u;
	return IOC_SHIM_HANDLE;
}
