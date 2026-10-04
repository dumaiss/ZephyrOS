/* ZephyrOS M7 diagnostic syscall service bodies (bank-7 kernel code).
 *
 * Six recovery/diagnostic services behind the unchanged M6 gate:
 *
 *   28h SYS_DIAG_MEM_READ    {bank, reserved, address}  -> byte in HL
 *   29h SYS_DIAG_MEM_WRITE   {bank, value, address}
 *   2Ah SYS_DIAG_IO_IN       {port}                     -> byte in HL
 *   2Bh SYS_DIAG_IO_OUT      {port, value}
 *   2Ch SYS_DIAG_BANK_INFO   (no block)                 -> mask:bank in HL
 *   2Dh SYS_DIAG_SYSTEM_INFO (record at block)          -> 8 in HL
 *
 * The services are explicitly recovery facilities: they validate the
 * complete request before any state changes, reject bank 7 for both
 * memory directions, protect the native page zero for writes, reject
 * the latch-aliased ports 00h-0Fh, and transfer through the M6 checked
 * bank-copy gate (exact latch/IFF/SP restoration).  A rejected request
 * leaves the process table, the selected bank, and every device
 * untouched.
 */

#include "zephyros/kernel.h"
#include "zephyros/process.h"

#include "kernel/memory/banking.h"

#include "diag.h"

/* Common-resident staging block owned by the copy contract. */
extern uint8_t zos_user_copy_staging[];

static uint16_t diag_read_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* One byte from `bank` at `address` through the checked copy gate. */
static uint8_t diag_bank_read(uint8_t bank, uint16_t address, uint8_t *out)
{
	struct zos_bank_copy_req req;

	req.bank = bank;
	req.op = ZOS_BANK_OP_READ;
	req.addr = address;
	req.buffer = zos_user_copy_staging;
	req.length = 1;
	if (zos_bank_copy(&req) != 0) {
		return 0;
	}
	*out = zos_user_copy_staging[0];
	return 1;
}

/* One byte to `bank` at `address` through the checked copy gate. */
static uint8_t diag_bank_write(uint8_t bank, uint16_t address, uint8_t value)
{
	struct zos_bank_copy_req req;

	zos_user_copy_staging[0] = value;
	req.bank = bank;
	req.op = ZOS_BANK_OP_WRITE;
	req.addr = address;
	req.buffer = zos_user_copy_staging;
	req.length = 1;
	return (uint8_t)(zos_bank_copy(&req) == 0);
}

static uint8_t diag_block_byte(uint16_t block, uint8_t offset)
{
	return ((const uint8_t *)block)[offset];
}

static uint16_t diag_block_word(uint16_t block, uint8_t offset)
{
	return diag_read_le16((const uint8_t *)block + offset);
}

void zos_diag_mem_read(uint16_t block)
{
	uint8_t bank;
	uint8_t reserved;
	uint16_t address;
	uint8_t value;

	if (!zos_syscall_block_ok(block,
				  (uint16_t)sizeof(struct
						   zos_diag_mem_read_args))) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	bank = diag_block_byte(block, 0);
	reserved = diag_block_byte(block, 1);
	address = diag_block_word(block, 2);
	if (bank > ZOS_DIAG_MEM_BANK_MAX || reserved != 0 ||
	    address > ZOS_DIAG_MEM_READ_MAX) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	if (!diag_bank_read(bank, address, &value)) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	zos_syscall_success(value);
}

void zos_diag_mem_write(uint16_t block)
{
	uint8_t bank;
	uint8_t value;
	uint16_t address;

	if (!zos_syscall_block_ok(block,
				  (uint16_t)sizeof(struct
						   zos_diag_mem_write_args))) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	bank = diag_block_byte(block, 0);
	value = diag_block_byte(block, 1);
	address = diag_block_word(block, 2);
	if (bank > ZOS_DIAG_MEM_BANK_MAX ||
	    address < ZOS_DIAG_MEM_WRITE_MIN ||
	    address > ZOS_DIAG_MEM_WRITE_MAX) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	if (!diag_bank_write(bank, address, value)) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	zos_syscall_success(0);
}

void zos_diag_io_in(uint16_t block)
{
	struct zos_diag_hw_port request;
	uint8_t port;

	if (!zos_syscall_block_ok(block,
				  (uint16_t)sizeof(struct
						   zos_diag_io_in_args))) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	port = diag_block_byte(block, 0);
	if (port < ZOS_DIAG_IO_PORT_MIN) {
		zos_syscall_error(ZOS_EINVAL); /* latch alias 00h-0Fh */
		return;
	}
	request.port = port;
	request.value = 0;
	zos_syscall_success(zos_diag_hw_in(&request));
}

void zos_diag_io_out(uint16_t block)
{
	struct zos_diag_hw_port request;
	uint8_t port;

	if (!zos_syscall_block_ok(block,
				  (uint16_t)sizeof(struct
						   zos_diag_io_out_args))) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	port = diag_block_byte(block, 0);
	if (port < ZOS_DIAG_IO_PORT_MIN) {
		zos_syscall_error(ZOS_EINVAL); /* latch alias 00h-0Fh */
		return;
	}
	request.port = port;
	request.value = diag_block_byte(block, 1);
	zos_diag_hw_out(&request);
	zos_syscall_success(0);
}

void zos_diag_bank_info(void)
{
	uint8_t bank = process_current_bank();

	/* Kernel context has no process descriptor; report the selected
	 * bank field instead, clamped to the application range. */
	if (bank > PROCESS_MAX_BANK) {
		bank = (uint8_t)(platform_memctl_read() & (uint8_t)ZOS_BANK_MASK);
		if (bank > PROCESS_MAX_BANK) {
			bank = 0;
		}
	}
	zos_syscall_success(((uint16_t)ZOS_DIAG_BANK_MASK << 8) | bank);
}

void zos_diag_system_info(uint16_t block)
{
	uint8_t *record;

	if (!zos_syscall_block_ok(block, ZOS_DIAG_SYSTEM_INFO_SIZE)) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	record = (uint8_t *)block;
	record[0] = (uint8_t)ZOS_DIAG_SYSTEM_INFO_SIZE;
	record[1] = (uint8_t)ZOS_DIAG_SYSTEM_INFO_ABI;
	record[2] = (uint8_t)ZOS_MODE_APPLICATION;
	record[3] = (uint8_t)ZOS_MODE_OS;
	record[4] = (uint8_t)(ZOS_COMMON_BASE & 0xFFu);
	record[5] = (uint8_t)(ZOS_COMMON_BASE >> 8);
	record[6] = (uint8_t)PROCESS_KERNEL_BANK;
	record[7] = (uint8_t)ZOS_ROMFS_FIRST_PAGE;
	zos_syscall_success(ZOS_DIAG_SYSTEM_INFO_SIZE);
}
