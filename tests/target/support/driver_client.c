/* KonaOS resident-driver acceptance client implementation (package
 * 41): see tests/target/include/driver_client.h for the contract. */

#include "driver_client.h"

#include "kernel.h"
#include "process.h"
#include "kos/protocol.h"

#include "kernel/ipc/ipc.h"
#include "kernel/vfs/vfs.h"

extern uint8_t kos_user_copy_staging[];

/* Class -> host-built publication name (architecture 36.13).  Package
 * 42b retired the class/instance lookup: a provider is named by its
 * publication, and the nucleus router answers the endpoint number the
 * packer resolved. */
static const char *driver_cli_publication(uint8_t class_id)
{
	switch (class_id) {
	case KOS_CLASS_CONSOLE: return "/console";
	case KOS_CLASS_SERIAL:  return "/serial/0";
	case KOS_CLASS_HID:     return "/input/keyboard";
	case KOS_CLASS_VIDEO:   return "/video";
	case KOS_CLASS_IOC:     return "/ioc";
	default:                return (const char *)0;
	}
}

uint8_t driver_cli_endpoint(uint8_t class_id, uint8_t *handle)
{
	const char *rel = driver_cli_publication(class_id);
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

int16_t driver_cli_call(uint8_t handle, struct kos_ipc_frame *frame)
{
	return ipc_nucleus_call(handle, frame);
}

void driver_cli_frame(struct kos_ipc_frame *frame, uint8_t op, uint8_t arg0,
		       uint16_t arg1)
{
	uint8_t index;

	frame->op = op;
	frame->flags = 0u;
	frame->desc = KOS_IPC_DESC_NONE;
	frame->arg0 = arg0;
	frame->arg1 = arg1;
	for (index = 0u; index < KOS_IPC_PAYLOAD_SIZE; ++index) {
		frame->payload[index] = 0u;
	}
}

static int stage(uint8_t op, uint16_t address, uint16_t length,
		 const uint8_t *pattern, uint8_t *out, uint16_t source_offset)
{
	uint16_t done = 0u;

	while (done < length) {
		struct kos_bank_copy_req request;
		uint16_t chunk = (uint16_t)(length - done);
		uint16_t index;

		if (chunk > DRIVER_CLI_STAGE) {
			chunk = DRIVER_CLI_STAGE;
		}
		if (op == KOS_BANK_OP_WRITE) {
			for (index = 0u; index < chunk; ++index) {
				kos_user_copy_staging[index] =
					pattern[(uint16_t)(source_offset +
							   done + index)];
			}
		}
		request.bank = DRIVER_CLI_BANK;
		request.op = op;
		request.addr = (uint16_t)(address + done);
		request.buffer = kos_user_copy_staging;
		request.length = chunk;
		if (kos_bank_copy(&request) != 0u) {
			return 0;
		}
		if (op == KOS_BANK_OP_READ && out != (uint8_t *)0) {
			for (index = 0u; index < chunk; ++index) {
				out[(uint16_t)(done + index)] =
					kos_user_copy_staging[index];
			}
		}
		done = (uint16_t)(done + chunk);
	}
	return 1;
}

int driver_cli_stage_read(uint16_t address, uint16_t length, uint8_t *dst)
{
	return stage(KOS_BANK_OP_READ, address, length, (const uint8_t *)0, dst,
		     0u);
}

int driver_cli_stage_write(uint16_t address, uint16_t length,
			    const uint8_t *src)
{
	return stage(KOS_BANK_OP_WRITE, address, length, src,
		     (uint8_t *)0, 0u);
}

int16_t driver_cli_putc(uint8_t handle, uint8_t op, uint8_t byte)
{
	struct kos_ipc_frame frame;

	driver_cli_frame(&frame, op, byte, 0u);
	return driver_cli_call(handle, &frame);
}

int16_t driver_cli_getc(uint8_t handle, uint8_t op)
{
	struct kos_ipc_frame frame;

	driver_cli_frame(&frame, op, 0u, 0u);
	return driver_cli_call(handle, &frame);
}

int16_t driver_cli_poll(uint8_t handle, uint8_t op)
{
	return driver_cli_getc(handle, op);
}

void driver_cli_puts(const char *text)
{
	early_console_puts(text);
}

void driver_cli_putchar(uint8_t byte)
{
	early_console_putc(byte);
}

/* Write the line to the acceptance console (the direct early SIO path) and
 * report the bytes written, which is what the groups' final console checks
 * compare against their line length. */
int driver_cli_write(const uint8_t *data, uint16_t length)
{
	uint16_t index;

	for (index = 0u; index < length; ++index) {
		early_console_putc(data[index]);
	}
	return (int)length;
}

uint8_t driver_cli_console_handle(void)
{
	uint8_t handle = 0u;

	(void)driver_cli_endpoint(KOS_CLASS_CONSOLE, &handle);
	return handle;
}

void driver_cli_hex8(uint8_t value)
{
	static const char digits[] = "0123456789ABCDEF";

	early_console_putc(digits[(value >> 4) & 0x0Fu]);
	early_console_putc(digits[value & 0x0Fu]);
}

void driver_cli_hex16(uint16_t value)
{
	driver_cli_hex8((uint8_t)(value >> 8));
	driver_cli_hex8((uint8_t)value);
}
