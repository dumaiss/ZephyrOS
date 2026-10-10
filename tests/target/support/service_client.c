/* ZephyrOS resident-service acceptance client implementation (package
 * 41): see tests/target/include/service_client.h for the contract. */

#include "service_client.h"

#include "zephyros/kernel.h"
#include "zephyros/process.h"
#include "zos/protocol.h"

#include "kernel/ipc/ipc.h"
#include "kernel/vfs/ns.h"

extern uint8_t zos_user_copy_staging[];

/* Class -> host-built publication name (architecture 36.13).  Package
 * 42b retired the class/instance lookup: a provider is named by its
 * publication, and the nucleus router answers the endpoint number the
 * packer resolved. */
static const char *service_cli_publication(uint8_t class_id)
{
	switch (class_id) {
	case ZOS_CLASS_CONSOLE: return "/console";
	case ZOS_CLASS_SERIAL:  return "/serial/0";
	case ZOS_CLASS_HID:     return "/input/keyboard";
	case ZOS_CLASS_VIDEO:   return "/video";
	case ZOS_CLASS_IOC:     return "/ioc";
	default:                return (const char *)0;
	}
}

uint8_t service_cli_endpoint(uint8_t class_id, uint8_t *handle)
{
	const char *rel = service_cli_publication(class_id);
	uint8_t number;

	if (rel == (const char *)0) {
		return 0u;
	}
	number = vfs_ns_publication_endpoint(rel);
	if (number == 0u) {
		return 0u;
	}
	if (handle != (uint8_t *)0) {
		*handle = number;
	}
	return 1u;
}

int16_t service_cli_call(uint8_t handle, struct zos_ipc_frame *frame)
{
	return ipc_nucleus_call(handle, frame);
}

void service_cli_frame(struct zos_ipc_frame *frame, uint8_t op, uint8_t arg0,
		       uint16_t arg1)
{
	uint8_t index;

	frame->op = op;
	frame->flags = 0u;
	frame->desc = ZOS_IPC_DESC_NONE;
	frame->arg0 = arg0;
	frame->arg1 = arg1;
	for (index = 0u; index < ZOS_IPC_PAYLOAD_SIZE; ++index) {
		frame->payload[index] = 0u;
	}
}

static int stage(uint8_t op, uint16_t address, uint16_t length,
		 const uint8_t *pattern, uint8_t *out, uint16_t source_offset)
{
	uint16_t done = 0u;

	while (done < length) {
		struct zos_bank_copy_req request;
		uint16_t chunk = (uint16_t)(length - done);
		uint16_t index;

		if (chunk > SERVICE_CLI_STAGE) {
			chunk = SERVICE_CLI_STAGE;
		}
		if (op == ZOS_BANK_OP_WRITE) {
			for (index = 0u; index < chunk; ++index) {
				zos_user_copy_staging[index] =
					pattern[(uint16_t)(source_offset +
							   done + index)];
			}
		}
		request.bank = SERVICE_CLI_BANK;
		request.op = op;
		request.addr = (uint16_t)(address + done);
		request.buffer = zos_user_copy_staging;
		request.length = chunk;
		if (zos_bank_copy(&request) != 0u) {
			return 0;
		}
		if (op == ZOS_BANK_OP_READ && out != (uint8_t *)0) {
			for (index = 0u; index < chunk; ++index) {
				out[(uint16_t)(done + index)] =
					zos_user_copy_staging[index];
			}
		}
		done = (uint16_t)(done + chunk);
	}
	return 1;
}

int service_cli_stage_read(uint16_t address, uint16_t length, uint8_t *dst)
{
	return stage(ZOS_BANK_OP_READ, address, length, (const uint8_t *)0, dst,
		     0u);
}

int service_cli_stage_write(uint16_t address, uint16_t length,
			    const uint8_t *src)
{
	return stage(ZOS_BANK_OP_WRITE, address, length, src,
		     (uint8_t *)0, 0u);
}

int16_t service_cli_putc(uint8_t handle, uint8_t op, uint8_t byte)
{
	struct zos_ipc_frame frame;

	service_cli_frame(&frame, op, byte, 0u);
	return service_cli_call(handle, &frame);
}

int16_t service_cli_getc(uint8_t handle, uint8_t op)
{
	struct zos_ipc_frame frame;

	service_cli_frame(&frame, op, 0u, 0u);
	return service_cli_call(handle, &frame);
}

int16_t service_cli_poll(uint8_t handle, uint8_t op)
{
	return service_cli_getc(handle, op);
}

void service_cli_puts(const char *text)
{
	early_console_puts(text);
}

void service_cli_putchar(uint8_t byte)
{
	early_console_putc(byte);
}

int service_cli_write(const uint8_t *data, uint16_t length)
{
	uint16_t index;

	for (index = 0u; index < length; ++index) {
		early_console_putc(data[index]);
	}
	return 0;
}

uint8_t service_cli_console_handle(void)
{
	uint8_t handle = 0u;

	(void)service_cli_endpoint(ZOS_CLASS_CONSOLE, &handle);
	return handle;
}

void service_cli_hex8(uint8_t value)
{
	static const char digits[] = "0123456789ABCDEF";

	early_console_putc(digits[(value >> 4) & 0x0Fu]);
	early_console_putc(digits[value & 0x0Fu]);
}

void service_cli_hex16(uint16_t value)
{
	service_cli_hex8((uint8_t)(value >> 8));
	service_cli_hex8((uint8_t)value);
}
