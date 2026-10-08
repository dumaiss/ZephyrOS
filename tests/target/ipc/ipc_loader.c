/* ZephyrOS IPC acceptance: the cold service loader (test artifact).
 *
 * Places the six synthetic service containers into bank 6 before the
 * client boots.  This is the stage-3 answer to "no loader stays in the
 * fixed core": the loader itself is cold nucleus code that runs with
 * P = 0, and it reaches bank 6 only through the two audited mechanisms -
 *
 *   - the common-resident copy gate (zos_bank_copy, mode 10 over bank 6)
 *     for the image payload, the BSS and the relocation patches;
 *   - the fixed upcall shim (zos_upcall) to run each service's init entry
 *     with the service page selected, its code in the window and the
 *     service bootstrap table in HL.
 *
 * Validation reuses the ZMOD validator (kernel/module/relocate.c) on the
 * linked container blob; the loader only places, patches and registers -
 * the real service-image format arrives in package 40.
 *
 * The loader stack is the common boot stack (visible whatever the window
 * shows); the staging buffer is the syscall copy gate's common buffer.
 */

#include "zephyros/errno.h"
#include "zephyros/ipc.h"
#include "zephyros/kernel.h"
#include "zephyros/module.h"
#include "zephyros/process.h"

#include "kernel/ipc/ipc.h"
#include "kernel/memory/upcall.h"
#include "kernel/syscall/diag.h"

#include "ipc_selftest.h"
#include "ipc_test.h"

/* Resident service containers, emitted by tools/zmodpack.py emit-asm
 * (build/ipc_blobs.asm) and linked into the fixed core. */
extern const uint8_t ipc_ioc_zmod[];
extern const uint16_t ipc_ioc_zmod_size;
extern const uint8_t ipc_fat_zmod[];
extern const uint16_t ipc_fat_zmod_size;
extern const uint8_t ipc_vfs_zmod[];
extern const uint16_t ipc_vfs_zmod_size;
extern const uint8_t ipc_echo_zmod[];
extern const uint16_t ipc_echo_zmod_size;
extern const uint8_t ipc_con_zmod[];
extern const uint16_t ipc_con_zmod_size;
extern const uint8_t ipc_irqx_zmod[];
extern const uint16_t ipc_irqx_zmod_size;

/* Bank 6 is the window bank for the service page (revision 13). */
#define IPC_SERVICE_BANK (uint8_t)(ZOS_BANK_MASK - ZOS_UPCALL_PAGE_SERVICE)

struct ipc_image {
	const char *name;
	const uint8_t *blob;
	const uint16_t *size;
};

/* Load order is the dependency order: a service resolves its peers by
 * class in its own init, so the provider must be registered first. */
static const struct ipc_image ipc_images[] = {
	{ "ioc", ipc_ioc_zmod, &ipc_ioc_zmod_size },
	{ "fat", ipc_fat_zmod, &ipc_fat_zmod_size },
	{ "vfs", ipc_vfs_zmod, &ipc_vfs_zmod_size },
	{ "echo", ipc_echo_zmod, &ipc_echo_zmod_size },
	{ "con", ipc_con_zmod, &ipc_con_zmod_size },
	{ "irqx", ipc_irqx_zmod, &ipc_irqx_zmod_size },
};

#define IPC_IMAGE_COUNT (sizeof(ipc_images) / sizeof(ipc_images[0]))

/* --- bank-6 access through the common copy gate ------------------------- */

static uint8_t bank_op(uint8_t op, uint16_t addr, uint16_t length)
{
	struct zos_bank_copy_req req;

	req.bank = IPC_SERVICE_BANK;
	req.op = op;
	req.addr = addr;
	req.buffer = zos_user_copy_staging;
	req.length = length;
	return zos_bank_copy(&req);
}

/* Copy `length` fixed-core bytes to a bank-6 address through the staging
 * buffer: the source is visible with P = 0, the staging buffer is common
 * and the gate switches to mode 10 for the destination. */
static uint8_t place_write(uint16_t dst, const uint8_t *src, uint16_t length)
{
	uint16_t done = 0u;

	while (done < length) {
		uint16_t chunk = (uint16_t)(length - done);
		uint16_t index;

		if (chunk > ZOS_SYSCALL_STAGE_SIZE) {
			chunk = ZOS_SYSCALL_STAGE_SIZE;
		}
		for (index = 0u; index < chunk; ++index) {
			zos_user_copy_staging[index] = src[done + index];
		}
		if (bank_op(ZOS_BANK_OP_WRITE, (uint16_t)(dst + done),
			    chunk) != 0u) {
			return 1u;
		}
		done = (uint16_t)(done + chunk);
	}
	return 0u;
}

static uint8_t place_zero(uint16_t dst, uint16_t length)
{
	return (length == 0u) ? 0u : bank_op(ZOS_BANK_OP_ZERO, dst, length);
}

/* Apply the validated ZREL_ABS16 records: word += load base, checked for
 * 16-bit overflow exactly like the kernel relocator. */
static uint8_t place_relocations(uint16_t base, const uint8_t *records,
				 uint16_t count, uint16_t image_size)
{
	uint16_t index;

	for (index = 0u; index < count; ++index) {
		const uint8_t *record = records + (uint16_t)(index << 2);
		uint16_t offset = (uint16_t)(record[2] |
					     ((uint16_t)record[3] << 8));
		uint16_t value;

		if (offset >= image_size ||
		    (uint16_t)(image_size - offset) < 2u) {
			return 1u;
		}
		if (bank_op(ZOS_BANK_OP_READ, (uint16_t)(base + offset),
			    2u) != 0u) {
			return 2u;
		}
		value = (uint16_t)(zos_user_copy_staging[0] |
				   ((uint16_t)zos_user_copy_staging[1] << 8));
		if (value > (uint16_t)(0xFFFFu - base)) {
			return 3u;
		}
		value = (uint16_t)(value + base);
		zos_user_copy_staging[0] = (uint8_t)value;
		zos_user_copy_staging[1] = (uint8_t)(value >> 8);
		if (bank_op(ZOS_BANK_OP_WRITE, (uint16_t)(base + offset),
			    2u) != 0u) {
			return 4u;
		}
	}
	return 0u;
}

/* Run one service init entry through the fixed upcall shim: page 1 shows
 * bank 6, the entry runs with the service bootstrap table in HL on the
 * loader's (visible) stack, and the shim restores the exact latch. */
static uint8_t place_start(uint16_t entry)
{
	struct zos_upcall_req req;

	req.page = ZOS_UPCALL_PAGE_SERVICE;
	req.entry = entry;
	req.args = (void *)ipc_service_api();
	req.result = 0;
	return zos_upcall(&req);
}

/* --- one container ------------------------------------------------------ */

static int ipc_blob_read(const void *ctx, uint16_t offset, uint8_t *dst,
			 uint16_t length)
{
	const uint8_t *src = (const uint8_t *)ctx + offset;
	uint16_t index;

	for (index = 0u; index < length; ++index) {
		dst[index] = src[index];
	}
	return (int)length;
}

static uint8_t place_image(const struct ipc_image *image, uint16_t *cursor)
{
	struct zmod_source source;
	struct zmod_container container;
	uint16_t used = (uint16_t)(IPC_SERVICE_BASE + *cursor);
	uint16_t base = (uint16_t)((used + (IPC_SERVICE_ALIGN - 1u)) &
				   ~(uint16_t)(IPC_SERVICE_ALIGN - 1u));

	source.read = ipc_blob_read;
	source.ctx = image->blob;
	source.size = *image->size;
	if (zmod_validate_source(&source, &container) != ZMOD_OK) {
		return 1u;
	}
	if ((uint32_t)base + container.image_size + container.bss_size >
	    ZOS_BANK7_RUNTIME_END) {
		return 2u;
	}
	/* Service I/O buffer rule (correction 01 to architecture 36.8): every
	 * container's data area must land inside bank 6's 8000h-BFFFh, the
	 * only bank-6 range the mode-11 window and the mode-10 aperture both
	 * show at the same address.  A container that outgrows it would put
	 * its copy_in/copy_out buffers on the chunked path for good. */
	if ((uint32_t)base + container.image_size + container.bss_size >
	    IPC_SERVICE_IO_END) {
		return 7u;
	}
	if (place_write(base,
			(const uint8_t *)(image->blob +
					  container.payload_offset),
			container.image_size) != 0u) {
		return 3u;
	}
	if (place_zero((uint16_t)(base + container.image_size),
		       container.bss_size) != 0u) {
		return 4u;
	}
	if (place_relocations(base,
			      (const uint8_t *)(image->blob +
						container.reloc_offset),
			      container.reloc_count,
			      container.image_size) != 0u) {
		return 5u;
	}
	if (place_start((uint16_t)(base + container.init_offset)) != 0u) {
		return 6u;
	}
	*cursor = (uint16_t)(base + container.image_size +
			     container.bss_size - IPC_SERVICE_BASE);
	return 0u;
}

int zos_ipc_services_load(void)
{
	uint16_t cursor = 0u;
	uint8_t index;
	uint8_t result = 0u;

	for (index = 0u; index < IPC_IMAGE_COUNT; ++index) {
		result = place_image(&ipc_images[index], &cursor);
		if (result != 0u) {
			early_console_puts("ipc loader: FAIL ");
			early_console_puts(ipc_images[index].name);
			early_console_puts(" code ");
			early_console_putc((char)('0' + result));
			early_console_puts("\r\n");
			return (int)result;
		}
	}
	early_console_puts("ipc loader: 6 services in bank 6\r\n");
	return 0;
}
