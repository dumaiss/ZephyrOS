/* ZephyrOS resident-service loader and boot manifest reader.
 *
 * Architecture 36.6, 36.11 and 36.12 (work package 40).  This cold-half
 * nucleus loader turns the service images the recovery store carries into
 * the bank-6 resident domain, in manifest order:
 *
 *   1. read the boot manifest (ZOS_SERVICE_MANIFEST_NAME) with the
 *      bootstrap ROMFS reader: a flat DATA file of 24-byte ROMFS name
 *      records, the load order;
 *   2. read every image's descriptors and bound-check them against the
 *      container before any bank-6 byte is touched (the packer validated
 *      the container on the host: M10-REFERENCE decision 2, so the loader
 *      checks the bounds that protect the kernel);
 *   3. reserve the service I/O blocks first (architecture 36.6): every
 *      image's io segment takes a slot inside 8000h-BFFFh, packed from
 *      C000h down, so the whole block is known before code is placed;
 *   4. assign the hot segments largest-first, bottom-up, never crossing
 *      the reserved io block, then the boot segments up from 0000h and
 *      the bulk data down from 6000h;
 *   5. in load order: check the declared dependencies against the endpoint
 *      table, place and relocate every segment, register the declared
 *      endpoints, run the boot segment once through the mode-10 gate with
 *      Q = 6, and run the hot init entry once through the fixed upcall
 *      shim with the service bootstrap table in HL.
 *
 * Relocation is the existing ZMOD model: the container carries the same
 * ZREL_ABS16 records, each patch offset relative to its own segment image
 * and its second byte naming the segment the patched value points into,
 * applied as `word += target segment base` with the 16-bit overflow check,
 * exactly as zmod_apply_relocations() (kernel/module/relocate.c) applies
 * the records to an arena module.  The record iteration has to go through
 * the common application-bank copy gate (zos_bank_copy, mode 10 over
 * bank 6) because bank 6 is not addressable from cold bank-7 code; the
 * format, the binding rule and the arithmetic are the existing ones.
 *
 * The loader keeps the two tiers' allocation cursors (the allocation map;
 * services are permanent, so nothing is ever freed) and the two
 * architecture-36.20 ledgers, `service hot` and `service boot/bulk`.
 *
 * Boot-tier code runs with bank 7 unmapped (see kernel/service/
 * service_boot.asm): the boot entry may touch bank 6, the common region
 * and I/O ports, and it must not call the bootstrap interface.  The hot
 * init entry runs in the mode-11 window, where the fixed core is visible,
 * so that is where a service resolves its dependencies and claims its
 * IRQs.
 */

#include "zephyros/service.h"
#include "zephyros/errno.h"
#include "zephyros/ipc.h"
#include "zephyros/kernel.h"
#include "zephyros/module.h"
#include "zephyros/process.h"
#include "zephyros/romfs.h"

#include "kernel/ipc/ipc.h"
#include "kernel/memory/upcall.h"
#include "kernel/syscall/diag.h"

/* The plan and ledger lines are acceptance evidence the replay pins
 * against the packer's report; production links only the error lines
 * (work package 41).  ZOS_TARGET_TEST_BUILD is the single production-side
 * guard (kernel/syscall.c carries the same preamble). */
#ifndef ZOS_TARGET_TEST_BUILD
#define ZOS_TARGET_TEST_BUILD 0
#endif

/* Bank 6 is the resident service domain (revision 13: bank = ZOS_BANK_MASK
 * - window page; the same derivation kernel/ipc/ipc.c uses). */
#define SERVICE_BANK (uint8_t)(ZOS_BANK_MASK - 1u)

/* Destination tiers (architecture 36.6). */
#define SERVICE_HOT_BASE  ZOS_REV13_WINDOW_BASE /* 6000h */
#define SERVICE_HOT_END   ZOS_BANK7_RUNTIME_END /* E000h */
#define SERVICE_IO_BASE   0x8000u
#define SERVICE_IO_END    0xC000u
#define SERVICE_BOOT_BASE 0x0000u
#define SERVICE_BOOT_END  ZOS_REV13_WINDOW_BASE /* 6000h */

/* The boot gate (kernel/service/service_boot.asm) and the upcall shim's
 * fixed-core request block and entry point (kernel/memory/upcall.h). */
extern uint8_t zos_service_run_boot(uint16_t entry);
extern struct zos_upcall_req zos_service_upcall_block;

/* Manifest entry N's logical offset: 24 = 16 + 8, computed with shifts
 * because the kernel links no multiply runtime (kernel/romfs/romfs.c). */
#define SERVICE_MANIFEST_AT(n) \
	(((uint32_t)(n) << 4) + ((uint32_t)(n) << 3))

/* One image's assignment, kept between the allocation pass and the load
 * pass.  The name and the container descriptors are re-read in the load
 * pass, so only the bases are remembered. */
struct service_image {
	uint16_t hot_size;  /* the hot segment's allocation, for the order */
	uint16_t io_base;   /* assigned in the io pass */
	uint16_t boot_base;
	uint16_t bulk_base;
	uint16_t hot_base;  /* 0 = not assigned yet (hot bases are >= 6000h) */
};

static struct service_image service_images[ZMOD_SERVICE_MAX_IMAGES];
static uint8_t service_image_count;
static uint8_t service_loader_ran;
static uint16_t service_hot_ledger;
static uint16_t service_boot_ledger;

/* Parsed container summary (cold data: the loader only runs with P = 0,
 * and single-tasking keeps it unambiguous). */
struct service_segment_info {
	uint8_t role;
	uint16_t image_size;
	uint16_t bss_size;
	uint16_t entry_offset;
	uint16_t payload_offset;
	uint16_t reloc_start;
	uint16_t reloc_count;
};

struct service_endpoint_info {
	uint8_t class;
	uint8_t instance;
	uint8_t version;
	uint8_t flags;
	uint16_t dispatch_offset;
};

struct service_layout {
	uint16_t image_size;
	uint16_t bss_size;
	uint16_t init_offset;
	uint16_t reloc_offset;
	uint16_t reloc_count;
	uint16_t payload_offset;
	uint16_t segment_offset;
	uint16_t endpoint_offset;
	uint16_t dependency_offset;
	uint8_t segment_count;
	uint8_t endpoint_count;
	uint8_t dependency_count;
	uint8_t hot_index;
	struct service_segment_info segment[ZMOD_SERVICE_MAX_SEGMENTS];
	struct service_endpoint_info endpoint[ZMOD_SERVICE_MAX_ENDPOINTS];
};

static struct service_layout service_layout;

/* The manifest name record currently read, reused for every image. */
static uint8_t service_name[ZOS_SERVICE_MANIFEST_ENTRY];

/* --- reporting (early console, written before any service exists) -------- */

static void service_puts(const char *text)
{
	early_console_puts(text);
}

#if ZOS_TARGET_TEST_BUILD
static void service_put_hex16(uint16_t value)
{
	uint8_t shift;
	static const char digits[] = "0123456789ABCDEF";

	for (shift = 16u; shift != 0u; ) {
		shift = (uint8_t)(shift - 4u);
		early_console_putc(digits[(value >> shift) & 0x0Fu]);
	}
	early_console_putc('h');
}
#endif /* ZOS_TARGET_TEST_BUILD */

/* Decimal without the SDCC division runtime (work/30b audit: the kernel
 * links no arithmetic helpers): repeated subtraction of the powers of
 * ten, which for a 16-bit value is at most 50 iterations. */
static void service_put_dec16(uint16_t value)
{
	static const uint16_t powers[5] = { 10000u, 1000u, 100u, 10u, 1u };
	uint8_t index;
	uint8_t started = 0u;

	for (index = 0u; index < 5u; ++index) {
		uint8_t digit = 0u;

		while (value >= powers[index]) {
			value = (uint16_t)(value - powers[index]);
			++digit;
		}
		if (digit != 0u || started != 0u || index == 4u) {
			started = 1u;
			early_console_putc((char)('0' + (char)digit));
		}
	}
}

/* One failure line: "service loader: <name>: <what>", the boot error the
 * contract requires on the early console. */
static void service_fail(const uint8_t *name, const char *what)
{
	service_puts("service loader: ");
	if (name != (const uint8_t *)0) {
		service_puts((const char *)name);
		service_puts(": ");
	}
	service_puts(what);
	service_puts("\r\n");
}

/* --- container reads ----------------------------------------------------- */

static uint16_t service_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static int service_source_read(const void *ctx, uint16_t offset, uint8_t *dst,
			       uint16_t length)
{
	const romfs_file_t *file = (const romfs_file_t *)ctx;

	return romfs_read(file, offset, dst, length);
}

static int service_read(const struct zmod_source *source, uint16_t offset,
			uint8_t *dst, uint16_t length)
{
	if ((uint32_t)offset + length > source->size) {
		return ZMOD_EFORMAT;
	}
	if (length == 0u) {
		return ZMOD_OK;
	}
	if (source->read(source->ctx, offset, dst, length) < 0) {
		return ZMOD_EFORMAT;
	}
	return ZMOD_OK;
}

/* A manifest name record: a non-empty printable-ASCII string inside the
 * ROMFS name field (the rule the ROMFS reader applies to stored names). */
static uint8_t service_name_ok(const uint8_t *name)
{
	uint8_t index;

	for (index = 0u; index < ZOS_SERVICE_MANIFEST_ENTRY; ++index) {
		uint8_t c = name[index];

		if (c == 0u) {
			return (index != 0u) ? 1u : 0u;
		}
		if (c < 0x21u || c > 0x7Eu) {
			return 0u;
		}
	}
	return 0u;
}

/* End of a table of `count` `size`-byte entries starting at `offset`,
 * saturating at 16 bits so a wrapped table can never look like it lies
 * inside the container. */
static uint16_t service_table_end(uint16_t offset, uint8_t size,
				  uint8_t count)
{
	uint32_t end = (uint32_t)offset + (uint16_t)(size * count);

	return (end > 0xFFFFu) ? 0xFFFFu : (uint16_t)end;
}

/* Read and bound-check one service container's descriptors.  The packer
 * validated the container on the host (M10-REFERENCE decision 2): the
 * loader reads the descriptors, checks the bounds that protect the kernel
 * (every table inside the container, every segment inside its tier, every
 * patch inside its segment) and copies.  What this accepts is exactly what
 * the placer and the relocator index with. */
static int service_read_layout(const struct zmod_source *source,
			       struct service_layout *layout)
{
	uint8_t header[ZMOD_HEADER_SIZE];
	uint8_t section[ZMOD_SERVICE_HEADER_SIZE];
	uint8_t record[ZMOD_SERVICE_SEGMENT_SIZE];
	uint8_t roles = 0u;
	uint16_t index;

	if (source->size < (uint16_t)(ZMOD_HEADER_SIZE +
				      ZMOD_SERVICE_HEADER_SIZE) ||
	    service_read(source, 0u, header, ZMOD_HEADER_SIZE) != ZMOD_OK) {
		return ZMOD_EFORMAT;
	}
	if (header[0] != ZMOD_MAGIC_0 || header[1] != ZMOD_MAGIC_1 ||
	    header[2] != ZMOD_MAGIC_2 || header[3] != ZMOD_MAGIC_3) {
		return ZMOD_EINVAL;
	}
	if (header[4] != ZMOD_FORMAT_VERSION ||
	    header[6] != ZMOD_SERVICE_ABI_VERSION) {
		return ZMOD_ENOTSUP;
	}
	if (header[5] != ZMOD_TYPE_SERVICE ||
	    header[7] != ZMOD_FLAG_NONE ||
	    service_le16(header + 0x0C) != 0u ||
	    service_le16(header + 0x10) != ZMOD_NO_ENTRY ||
	    service_le16(header + 0x18) != source->size ||
	    service_le16(header + 0x1A) != 0u) {
		return ZMOD_EINVAL;
	}
	layout->image_size = service_le16(header + 0x08);
	layout->bss_size = service_le16(header + 0x0A);
	layout->init_offset = service_le16(header + 0x0E);
	layout->reloc_offset = service_le16(header + 0x12);
	layout->reloc_count = service_le16(header + 0x14);
	layout->payload_offset = service_le16(header + 0x16);
	/* One relocation table below the payload, the payload reaching the
	 * container end, no field wrapping 16 bits. */
	if (layout->reloc_offset < (uint16_t)(ZMOD_HEADER_SIZE +
					      ZMOD_SERVICE_HEADER_SIZE) ||
	    layout->payload_offset < (uint16_t)(ZMOD_HEADER_SIZE +
						ZMOD_SERVICE_HEADER_SIZE) ||
	    layout->reloc_count >
	    (uint16_t)((uint16_t)(0xFFFFu - layout->reloc_offset) >> 2) ||
	    (uint16_t)(layout->reloc_offset +
		       (uint16_t)(layout->reloc_count << 2)) >
	    layout->payload_offset ||
	    (uint16_t)(layout->payload_offset + layout->image_size) !=
	    source->size) {
		return ZMOD_EFORMAT;
	}
	if (service_read(source, ZMOD_SERVICE_SECTION_OFFSET, section,
			 ZMOD_SERVICE_HEADER_SIZE) != ZMOD_OK) {
		return ZMOD_EFORMAT;
	}
	layout->segment_count = section[0x08];
	layout->endpoint_count = section[0x09];
	layout->dependency_count = section[0x0A];
	layout->segment_offset = service_le16(section + 0x02);
	layout->endpoint_offset = service_le16(section + 0x04);
	layout->dependency_offset = service_le16(section + 0x06);
	if (layout->segment_count == 0u ||
	    layout->segment_count > ZMOD_SERVICE_MAX_SEGMENTS ||
	    layout->endpoint_count > ZMOD_SERVICE_MAX_ENDPOINTS ||
	    layout->dependency_count > ZMOD_SERVICE_MAX_DEPENDENCIES) {
		return ZMOD_EFORMAT;
	}
	/* The section's tables sit between its header and the relocation
	 * table (the section starts directly after the container header). */
	if (service_table_end(layout->segment_offset,
			      ZMOD_SERVICE_SEGMENT_SIZE,
			      layout->segment_count) > layout->reloc_offset ||
	    service_table_end(layout->endpoint_offset,
			      ZMOD_SERVICE_ENDPOINT_SIZE,
			      layout->endpoint_count) >
	    layout->reloc_offset ||
	    service_table_end(layout->dependency_offset,
			      ZMOD_SERVICE_DEPENDENCY_SIZE,
			      layout->dependency_count) >
	    layout->reloc_offset) {
		return ZMOD_EFORMAT;
	}

	layout->hot_index = 0xFFu;
	for (index = 0u; index < layout->segment_count; ++index) {
		struct service_segment_info *segment = &layout->segment[index];
		uint8_t role_bit;

		if (service_read(source,
				 (uint16_t)(ZMOD_SERVICE_SECTION_OFFSET +
					    layout->segment_offset +
					    (uint16_t)(ZMOD_SERVICE_SEGMENT_SIZE *
						       index)),
				 record, ZMOD_SERVICE_SEGMENT_SIZE) !=
		    ZMOD_OK) {
			return ZMOD_EFORMAT;
		}
		if (record[0] == 0u || record[0] > ZMOD_SEGMENT_BULK) {
			return ZMOD_EINVAL;
		}
		role_bit = (uint8_t)(1u << record[0]);
		if ((roles & role_bit) != 0u) {
			return ZMOD_EFORMAT; /* one segment per role */
		}
		roles = (uint8_t)(roles | role_bit);
		segment->role = record[0];
		segment->image_size = service_le16(record + 0x02);
		segment->bss_size = service_le16(record + 0x04);
		segment->entry_offset = service_le16(record + 0x06);
		segment->payload_offset = service_le16(record + 0x08);
		segment->reloc_start = service_le16(record + 0x0A);
		segment->reloc_count = service_le16(record + 0x0C);
		if (segment->image_size == 0u && segment->bss_size == 0u) {
			return ZMOD_EFORMAT;
		}
		if ((uint32_t)segment->payload_offset +
		    segment->image_size > layout->image_size ||
		    (uint32_t)segment->reloc_start + segment->reloc_count >
		    layout->reloc_count) {
			return ZMOD_EFORMAT;
		}
		if (segment->entry_offset != ZMOD_NO_ENTRY &&
		    (segment->role != ZMOD_SEGMENT_BOOT ||
		     segment->entry_offset >= segment->image_size)) {
			return ZMOD_EFORMAT;
		}
		if (segment->role == ZMOD_SEGMENT_HOT) {
			layout->hot_index = index;
		}
	}
	if (layout->hot_index == 0xFFu ||
	    (layout->init_offset != ZMOD_NO_ENTRY &&
	     layout->init_offset >=
	     layout->segment[layout->hot_index].image_size)) {
		return ZMOD_EFORMAT;
	}

	for (index = 0u; index < layout->endpoint_count; ++index) {
		struct service_endpoint_info *endpoint =
			&layout->endpoint[index];
		uint16_t dispatch;

		if (service_read(source,
				 (uint16_t)(ZMOD_SERVICE_SECTION_OFFSET +
					    layout->endpoint_offset +
					    (uint16_t)(ZMOD_SERVICE_ENDPOINT_SIZE *
						       index)),
				 record, ZMOD_SERVICE_ENDPOINT_SIZE) !=
		    ZMOD_OK) {
			return ZMOD_EFORMAT;
		}
		dispatch = service_le16(record + 0x04);
		if (service_le16(record + 0x06) != 0u ||
		    (record[3] & (uint8_t)~ZMOD_SERVICE_EP_REENTRANT) != 0u ||
		    dispatch >= layout->segment[layout->hot_index].image_size) {
			return ZMOD_EINVAL;
		}
		endpoint->class = record[0];
		endpoint->instance = record[1];
		endpoint->version = record[2];
		endpoint->flags = record[3];
		endpoint->dispatch_offset = dispatch;
	}
	return ZMOD_OK;
}
/* --- bank-6 access through the common copy gate -------------------------- */

static uint8_t service_bank_op(uint8_t op, uint16_t addr, uint16_t length)
{
	struct zos_bank_copy_req req;

	req.bank = SERVICE_BANK;
	req.op = op;
	req.addr = addr;
	req.buffer = zos_user_copy_staging;
	req.length = length;
	return zos_bank_copy(&req);
}

/* Copy `length` container bytes to bank-6 `dst` through the common
 * staging buffer and the gate (which validates the bank and restores the
 * exact latch, stack and interrupt state). */
static int service_place(const struct zmod_source *source, uint16_t offset,
			 uint16_t dst, uint16_t length)
{
	uint16_t done = 0u;

	while (done < length) {
		uint16_t chunk = (uint16_t)(length - done);

		if (chunk > ZOS_SYSCALL_STAGE_SIZE) {
			chunk = ZOS_SYSCALL_STAGE_SIZE;
		}
		if (service_read(source, (uint16_t)(offset + done),
				 zos_user_copy_staging, chunk) != ZMOD_OK) {
			return ZMOD_EFORMAT;
		}
		if (service_bank_op(ZOS_BANK_OP_WRITE,
				    (uint16_t)(dst + done), chunk) != 0u) {
			return ZMOD_EFORMAT;
		}
		done = (uint16_t)(done + chunk);
	}
	return ZMOD_OK;
}

/* Apply one segment's ZREL_ABS16 records through the gate: the patch site
 * is `own_base + patch`, the same rule and overflow check
 * zmod_apply_relocations() uses (`word += target segment base`), with each
 * record's patch offset relative to the segment that owns it and its
 * second byte naming the segment the patched value points into.  The
 * record validation and the patch arithmetic are the shared
 * kernel/module/relocate.c logic (work package 41); only the byte-1
 * target rule and the gate-based patch I/O are service-specific.  A
 * record whose target or patch is outside the segment aborts before
 * anything is written back. */
static int service_relocate(const struct zmod_source *source,
			    const struct service_segment_info *segment,
			    uint16_t own_base, const uint16_t *bases)
{
	uint16_t index;

	for (index = 0u; index < segment->reloc_count; ++index) {
		uint8_t record[ZMOD_RELOC_SIZE];
		uint16_t at = (uint16_t)(service_layout.reloc_offset +
			(uint16_t)((uint16_t)(segment->reloc_start + index) << 2));
		uint16_t patch;
		uint16_t value;
		uint8_t overflow;
		int result;

		if (service_read(source, at, record, ZMOD_RELOC_SIZE) !=
		    ZMOD_OK) {
			return ZMOD_EFORMAT;
		}
		result = zmod_reloc_decode(record[0],
					   service_le16(record + 2),
					   segment->image_size, &patch);
		if (result != ZMOD_OK || record[1] >= service_layout.segment_count) {
			return ZMOD_EFORMAT;
		}
		if ((uint32_t)own_base + patch + 2u > 0x10000u) {
			return ZMOD_EFORMAT;
		}
		if (service_bank_op(ZOS_BANK_OP_READ,
				    (uint16_t)(own_base + patch), 2u) != 0u) {
			return ZMOD_EFORMAT;
		}
		value = service_le16(zos_user_copy_staging);
		value = zmod_reloc_add(value, bases[record[1]], &overflow);
		if (overflow != 0u) {
			return ZMOD_EFORMAT; /* relocation overflow */
		}
		zos_user_copy_staging[0] = (uint8_t)value;
		zos_user_copy_staging[1] = (uint8_t)(value >> 8);
		if (service_bank_op(ZOS_BANK_OP_WRITE,
				    (uint16_t)(own_base + patch), 2u) != 0u) {
			return ZMOD_EFORMAT;
		}
	}
	return ZMOD_OK;
}

/* --- registration, dependencies and bring-up ----------------------------- */

static int service_register_endpoints(const struct service_layout *layout,
				      uint16_t hot_base,
				      const struct ipc_service_api *api)
{
	uint8_t index;

	for (index = 0u; index < layout->endpoint_count; ++index) {
		const struct service_endpoint_info *ep = &layout->endpoint[index];

		if (api->register_ep(ep->class, ep->instance,
				     (uint16_t)(hot_base +
						ep->dispatch_offset),
				     ep->flags, ep->version) <= 0) {
			service_fail(service_name, "endpoint refused");
			return ZMOD_ENOSPC;
		}
	}
	return ZMOD_OK;
}

/* Every declared dependency must already be registered, which the manifest
 * order guarantees for a valid boot set (the packer validates the same
 * closure).  A missing dependency is a boot error reported on the early
 * console. */
static int service_check_dependencies(const struct zmod_source *source,
				      const struct service_layout *layout,
				      const struct ipc_service_api *api)
{
	uint8_t index;

	for (index = 0u; index < layout->dependency_count; ++index) {
		uint8_t record[ZMOD_SERVICE_DEPENDENCY_SIZE];
		uint16_t at = (uint16_t)(ZMOD_SERVICE_SECTION_OFFSET +
			layout->dependency_offset +
			(uint16_t)(ZMOD_SERVICE_DEPENDENCY_SIZE * index));

		if (service_read(source, at, record,
				 ZMOD_SERVICE_DEPENDENCY_SIZE) != ZMOD_OK) {
			return ZMOD_EFORMAT;
		}
		if (api->lookup(record[0], record[1]) == 0) {
			service_puts("service loader: ");
			service_puts((const char *)service_name);
			service_puts(": missing dependency ");
			service_put_dec16(record[0]);
			service_puts(".");
			service_put_dec16(record[1]);
			service_puts("\r\n");
			return ZMOD_ENOENT;
		}
	}
	return ZMOD_OK;
}

static int service_run_boot(const struct service_layout *layout,
			    uint16_t boot_base)
{
	const struct service_segment_info *segment;
	uint8_t index;

	for (index = 0u; index < layout->segment_count; ++index) {
		if (layout->segment[index].role != ZMOD_SEGMENT_BOOT) {
			continue;
		}
		segment = &layout->segment[index];
		if (segment->entry_offset == ZMOD_NO_ENTRY) {
			continue; /* a boot/bulk data segment runs nothing */
		}
		if (zos_service_run_boot((uint16_t)(boot_base +
						   segment->entry_offset)) !=
		    0u) {
			service_fail(service_name, "boot refused");
			return ZMOD_EINVAL;
		}
	}
	return ZMOD_OK;
}

/* The hot init entry, once, through the fixed upcall shim: the mode-11
 * window shows bank 6 and the fixed core stays visible, so the service
 * resolves its dependencies and claims its IRQs here with the bootstrap
 * table in HL. */
static int service_run_init(const struct service_layout *layout,
			    uint16_t hot_base,
			    const struct ipc_service_api *api)
{
	if (layout->init_offset == ZMOD_NO_ENTRY) {
		return ZMOD_OK;
	}
	zos_service_upcall_block.page = ZOS_UPCALL_PAGE_SERVICE;
	zos_service_upcall_block.entry = (uint16_t)(hot_base +
						    layout->init_offset);
	zos_service_upcall_block.args = (void *)api;
	zos_service_upcall_block.result = 0u;
	if (zos_upcall(&zos_service_upcall_block) != 0u) {
		service_fail(service_name, "init refused");
		return ZMOD_EINVAL;
	}
	return ZMOD_OK;
}

static uint16_t service_align2(uint16_t value)
{
	return (uint16_t)((value + 1u) & ~1u);
}

/* A block placed below a descending cursor starts its own size below it,
 * rounded *down* to an even address, so the next block below can never
 * overlap it (tools/zmodpack.py's ServiceTierPlan applies the same rule). */
static uint16_t service_align2_down(uint16_t value)
{
	return (uint16_t)(value & ~1u);
}

/* --- the loader ---------------------------------------------------------- */

/* Open manifest entry `index` as a container: read its name record, resolve
 * it in the recovery store and bind the read source to it. */
static int service_open(const romfs_file_t *manifest, uint16_t index,
			romfs_file_t *file, struct zmod_source *source)
{
	if (romfs_read(manifest, SERVICE_MANIFEST_AT(index), service_name,
		       ZOS_SERVICE_MANIFEST_ENTRY) !=
	    (int)ZOS_SERVICE_MANIFEST_ENTRY ||
	    !service_name_ok(service_name)) {
		service_fail((const uint8_t *)0, "bad manifest entry");
		return ZMOD_EFORMAT;
	}
	if (romfs_lookup_ci((const char *)service_name, file) != 0 ||
	    file->flags != ROMFS_FLAG_ZMOD || file->size > 0xFFFFu) {
		service_fail(service_name, "no image");
		return ZMOD_ENOENT;
	}
	source->read = service_source_read;
	source->ctx = file;
	source->size = (uint16_t)file->size;
	return ZMOD_OK;
}

/* One image's assigned base for `role`, if it has that segment (evidence
 * only: the plan line). */
#if ZOS_TARGET_TEST_BUILD
static uint8_t service_role_base(uint8_t role,
				 const struct service_image *image,
				 uint16_t *base)
{
	uint8_t index;

	for (index = 0u; index < service_layout.segment_count; ++index) {
		if (service_layout.segment[index].role != role) {
			continue;
		}
		if (role == ZMOD_SEGMENT_IO) {
			*base = image->io_base;
		} else if (role == ZMOD_SEGMENT_BOOT) {
			*base = image->boot_base;
		} else {
			*base = image->bulk_base;
		}
		return 1u;
	}
	return 0u;
}
#endif /* ZOS_TARGET_TEST_BUILD */

/* The plan line the acceptance transcript pins: the roles in the order
 * tools/zmodpack.py's `services` command prints them, absent roles left
 * out.  Evidence only: production does not link it. */
#if ZOS_TARGET_TEST_BUILD
static void service_report(const struct service_image *image)
{
	static const uint8_t roles[3] = {
		ZMOD_SEGMENT_IO, ZMOD_SEGMENT_BOOT, ZMOD_SEGMENT_BULK
	};
	static const char *const names[3] = { " io ", " boot ", " bulk " };
	uint8_t index;

	service_puts("service ");
	service_puts((const char *)service_name);
	service_puts(": hot ");
	service_put_hex16(image->hot_base);
	for (index = 0u; index < 3u; ++index) {
		uint16_t base;

		if (service_role_base(roles[index], image, &base)) {
			service_puts(names[index]);
			service_put_hex16(base);
		}
	}
	service_puts("\r\n");
}
#endif /* ZOS_TARGET_TEST_BUILD */

int service_load_all(const char *manifest_name)
{
	romfs_file_t manifest;
	romfs_file_t file;
	struct zmod_source source;
	const struct ipc_service_api *api = ipc_service_api();
	uint16_t entries;
	uint16_t io_cursor = SERVICE_IO_END;
	uint16_t boot_cursor = SERVICE_BOOT_BASE;
	uint16_t bulk_cursor = SERVICE_BOOT_END;
	uint16_t index;
	int result;

	if (service_loader_ran) {
		return ZMOD_OK; /* services are permanent: load once */
	}
	if (manifest_name == (const char *)0 ||
	    romfs_lookup_ci(manifest_name, &manifest) != 0) {
		service_fail((const uint8_t *)0, "no manifest");
		return ZMOD_ENOENT;
	}
	if (manifest.flags != ROMFS_FLAG_DATA ||
	    manifest.size > 0xFFFFu ||
	    manifest.size == 0u) {
		service_fail((const uint8_t *)0, "bad manifest");
		return ZMOD_EFORMAT;
	}
	/* Entry count by repeated subtraction: the kernel links no SDCC
	 * division helpers (work/30b audit). */
	entries = 0u;
	{
		uint16_t remaining = manifest.size;

		while (remaining >= ZOS_SERVICE_MANIFEST_ENTRY) {
			remaining = (uint16_t)(remaining -
					       ZOS_SERVICE_MANIFEST_ENTRY);
			++entries;
		}
		if (remaining != 0u) {
			service_fail((const uint8_t *)0,
				     "bad manifest");
			return ZMOD_EFORMAT;
		}
	}
	if (entries > ZMOD_SERVICE_MAX_IMAGES) {
		service_fail((const uint8_t *)0, "too many images");
		return ZMOD_ENOSPC;
	}
	service_image_count = (uint8_t)entries;

	/* First pass, in load order: read every image's descriptors and
	 * assign its service I/O block, packed from C000h down so the whole
	 * block is known before any hot code is placed (architecture 36.6),
	 * then its boot and bulk space (boot up from 0000h, bulk down from
	 * 6000h, each block rounded down to an even address so it can never
	 * overlap the next one).  The two 36.20 ledgers count what the tiers
	 * hold. */
	for (index = 0u; index < entries; ++index) {
		struct service_image *image = &service_images[index];
		uint8_t segment_index;

		result = service_open(&manifest, index, &file, &source);
		if (result != ZMOD_OK) {
			return result;
		}
		result = service_read_layout(&source, &service_layout);
		if (result != ZMOD_OK) {
			service_fail(service_name, "invalid image");
			return result;
		}
		for (segment_index = 0u;
		     segment_index < service_layout.segment_count;
		     ++segment_index) {
			const struct service_segment_info *segment =
				&service_layout.segment[segment_index];
			uint16_t size = (uint16_t)(segment->image_size +
						   segment->bss_size);
			uint16_t base;

			if (segment->role == ZMOD_SEGMENT_HOT) {
				image->hot_size = size;
				service_hot_ledger = (uint16_t)(
					service_hot_ledger + size);
			} else if (segment->role == ZMOD_SEGMENT_IO) {
				io_cursor = service_align2_down(
					(uint16_t)(io_cursor - size));
				if (io_cursor < SERVICE_IO_BASE) {
					service_fail(service_name,
						     "io range full");
					return ZMOD_ENOSPC;
				}
				image->io_base = io_cursor;
				service_hot_ledger = (uint16_t)(
					service_hot_ledger + size);
			} else if (segment->role == ZMOD_SEGMENT_BOOT) {
				base = service_align2(boot_cursor);
				if ((uint32_t)base + size > bulk_cursor) {
					service_fail(service_name,
						     "boot/bulk full");
					return ZMOD_ENOSPC;
				}
				image->boot_base = base;
				boot_cursor = (uint16_t)(base + size);
				service_boot_ledger = (uint16_t)(
					service_boot_ledger + size);
			} else {
				base = service_align2_down(
					(uint16_t)(bulk_cursor - size));
				if (base < boot_cursor) {
					service_fail(service_name,
						     "boot/bulk full");
					return ZMOD_ENOSPC;
				}
				image->bulk_base = base;
				bulk_cursor = base;
				service_boot_ledger = (uint16_t)(
					service_boot_ledger + size);
			}
		}
	}

	/* Hot segments largest-first: each takes the lowest free run of the
	 * hot tier that does not cross the reserved io block: below it when
	 * it fits, otherwise just above C000h. */
	{
		uint16_t hot_cursor = SERVICE_HOT_BASE;
		uint8_t placed;

		for (placed = 0u; placed < service_image_count; ++placed) {
			uint8_t best = 0xFFu;
			uint16_t best_size = 0u;
			uint16_t base;

			for (index = 0u; index < service_image_count;
			     ++index) {
				if (service_images[index].hot_base != 0u ||
				    service_images[index].hot_size <=
				    best_size) {
					continue;
				}
				best = (uint8_t)index;
				best_size = service_images[index].hot_size;
			}
			if (best == 0xFFu) {
				break;
			}
			base = service_align2(hot_cursor);
			if ((uint16_t)(base + best_size) > io_cursor &&
			    base < SERVICE_IO_END) {
				base = SERVICE_IO_END;
			}
			if ((uint32_t)base + best_size > SERVICE_HOT_END) {
				service_fail((const uint8_t *)0,
					     "hot tier full");
				return ZMOD_ENOSPC;
			}
			service_images[best].hot_base = base;
			hot_cursor = (uint16_t)(base + best_size);
		}
	}

	/* Load pass, in manifest order: dependencies against the endpoint
	 * table, placement and relocation of every segment, endpoint
	 * registration, the boot segment once through the mode-10 gate, then
	 * the hot init entry once through the fixed upcall shim. */
	for (index = 0u; index < entries; ++index) {
		const struct service_image *image = &service_images[index];
		uint16_t bases[ZMOD_SERVICE_MAX_SEGMENTS];
		uint8_t segment_index;

		result = service_open(&manifest, index, &file, &source);
		if (result == ZMOD_OK) {
			result = service_read_layout(&source,
						     &service_layout);
		}
		if (result != ZMOD_OK) {
			service_fail(service_name, "invalid image");
			return result;
		}
		result = service_check_dependencies(&source, &service_layout,
						    api);
		if (result != ZMOD_OK) {
			return result;
		}
		/* Every segment first, then the relocations: a record may
		 * name a segment that comes later in the table. */
		for (segment_index = 0u;
		     segment_index < service_layout.segment_count;
		     ++segment_index) {
			const struct service_segment_info *segment =
				&service_layout.segment[segment_index];
			uint16_t base;

			if (segment->role == ZMOD_SEGMENT_HOT) {
				base = image->hot_base;
			} else if (segment->role == ZMOD_SEGMENT_IO) {
				base = image->io_base;
			} else if (segment->role == ZMOD_SEGMENT_BOOT) {
				base = image->boot_base;
			} else {
				base = image->bulk_base;
			}
			bases[segment_index] = base;
			result = service_place(
				&source,
				(uint16_t)(service_layout.payload_offset +
					   segment->payload_offset),
				base, segment->image_size);
			if (result == ZMOD_OK && segment->bss_size != 0u &&
			    service_bank_op(ZOS_BANK_OP_ZERO,
					    (uint16_t)(base +
						       segment->image_size),
					    segment->bss_size) != 0u) {
				result = ZMOD_EFORMAT;
			}
			if (result != ZMOD_OK) {
				service_fail(service_name, "copy refused");
				return result;
			}
		}
		for (segment_index = 0u;
		     segment_index < service_layout.segment_count;
		     ++segment_index) {
			result = service_relocate(
				&source,
				&service_layout.segment[segment_index],
				bases[segment_index], bases);
			if (result != ZMOD_OK) {
				service_fail(service_name, "copy refused");
				return result;
			}
		}
		/* The image's bulk extent travels with its endpoints: the
		 * run-time bulk copy (36.6) is bound to the service that
		 * owns the endpoint being dispatched, and this is the one
		 * place the loader knows both. */
		{
			uint16_t bulk_base = 0u;
			uint16_t bulk_size = 0u;

			for (segment_index = 0u;
			     segment_index < service_layout.segment_count;
			     ++segment_index) {
				const struct service_segment_info *segment =
					&service_layout.segment[segment_index];

				if (segment->role == ZMOD_SEGMENT_BULK) {
					bulk_base = image->bulk_base;
					bulk_size = (uint16_t)(
						segment->image_size +
						segment->bss_size);
				}
			}
			ipc_service_bulk_set(bulk_base, bulk_size);
		}
		result = service_register_endpoints(&service_layout,
						    image->hot_base, api);
		if (result == ZMOD_OK) {
			result = service_run_boot(&service_layout,
						  image->boot_base);
		}
		if (result == ZMOD_OK) {
			result = service_run_init(&service_layout,
						  image->hot_base, api);
		}
		if (result != ZMOD_OK) {
			return result;
		}
#if ZOS_TARGET_TEST_BUILD
		service_report(image);
#endif
	}

	service_loader_ran = 1u;
#if ZOS_TARGET_TEST_BUILD
	/* Evidence only: the ledger tail the replay pins against the
	 * packer's report. */
	service_puts("service loader: ");
	service_put_dec16((uint16_t)entries);
	service_puts(" images in bank 6\r\n");
	service_puts("service hot: ");
	service_put_dec16(service_hot_ledger);
	service_puts(" bytes of 32768\r\n");
	service_puts("service boot/bulk: ");
	service_put_dec16(service_boot_ledger);
	service_puts(" bytes of 24576\r\n");
#endif
	return ZMOD_OK;
}



uint8_t service_images_loaded(void)
{
	return service_loader_ran;
}

uint16_t service_hot_used(void)
{
	return service_hot_ledger;
}

uint16_t service_boot_used(void)
{
	return service_boot_ledger;
}
