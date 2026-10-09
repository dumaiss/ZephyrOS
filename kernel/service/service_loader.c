/* ZephyrOS resident-driver loader and boot manifest reader.
 *
 * Architecture 36.6, 36.11 and 36.12 (work packages 40-41b).  This
 * cold-half nucleus loader turns the driver images the recovery store
 * carries into the bank-6 resident domain, in manifest order:
 *
 *   1. read the boot manifest (ZOS_SERVICE_MANIFEST_NAME) with the
 *      bootstrap ROMFS reader: a flat DATA file of 24-byte ROMFS name
 *      records, the load order;
 *   2. for each image, read its descriptors and bound-check them against
 *      the container and against the fixed bank-6 layout: every segment's
 *      final base lies in its tier, every entry and dispatch address lies
 *      inside its segment, every table lies inside the container (the
 *      packer validated all of this on the host: M10-REFERENCE decision
 *      2, so the loader checks only the bounds that protect the nucleus);
 *   3. copy every segment image to its host-assigned base and zero its
 *      BSS, through the common application-bank copy gate;
 *   4. install the image's host-built endpoint records into the nucleus
 *      endpoint table, with the image's bulk extent, and install the
 *      resolved dependency numbers of this image;
 *   5. run the boot segment once through the mode-10 gate with Q = 6, and
 *      run the hot init entry once through the fixed upcall shim with the
 *      boot bundle (interface table and dependency array) in HL.
 *
 * There is no relocation, no placement policy and no dependency check
 * here: the host linked the images at their final addresses and resolved
 * the endpoints and dependencies (architecture 36.11).  The loader keeps
 * the two architecture-36.20 ledgers, `driver hot` (hot plus io bytes)
 * and `driver boot/bulk` (boot plus bulk bytes).
 *
 * Boot-tier code runs with bank 7 unmapped (see kernel/service/
 * service_boot.asm): the boot entry may touch bank 6, the common region
 * and I/O ports, and it must not call the bootstrap interface.
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
#if ZOS_NS_HANDLES
#include "kernel/vfs/ns.h"
#endif

/* The plan and ledger lines are acceptance evidence the replay pins
 * against the packer's report; production links only the error lines
 * (work package 41).  Target images normally inherit the shared
 * ZOS_TARGET_TEST_BUILD guard.  The M9 integration image suppresses only
 * this verbose evidence so its IOC-FAT arena remains production-sized.
 * kernel/syscall.c carries the shared-guard preamble too. */
#ifndef ZOS_TARGET_TEST_BUILD
#define ZOS_TARGET_TEST_BUILD 0
#endif
#ifndef ZOS_SERVICE_TEST_EVIDENCE
#define ZOS_SERVICE_TEST_EVIDENCE ZOS_TARGET_TEST_BUILD
#endif

/* Bank 6 is the resident driver domain (revision 13: bank = ZOS_BANK_MASK
 * - window page; the same derivation kernel/ipc/ipc.c uses). */
#define SERVICE_BANK (uint8_t)(ZOS_BANK_MASK - 1u)

/* Destination tiers (architecture 36.6): the hot tier 6000h-DFFFh (the io
 * role lives inside 8000h-BFFFh), the boot/bulk tier 0000h-5FFFh.  The
 * host computed each address; these are the ranges the loader checks. */
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

static uint8_t service_loader_ran;
static uint16_t service_hot_ledger;
static uint16_t service_boot_ledger;

/* Parsed container summary (cold data: the loader only runs with P = 0,
 * and single-tasking keeps it unambiguous). */
struct service_segment_info {
	uint8_t role;
	uint8_t flags;
	uint16_t base;
	uint16_t image_size;
	uint16_t bss_size;
	uint16_t entry;
	uint16_t payload_offset;
	uint16_t reserved;
};

struct service_endpoint_info {
	uint8_t class;
	uint8_t instance;
	uint8_t version;
	uint8_t flags;
	uint16_t number;
	uint16_t dispatch;
};

struct service_layout {
	uint16_t image_size;
	uint16_t bss_size;
	uint16_t init;
	uint16_t payload_offset;
	uint16_t segment_offset;
	uint16_t endpoint_offset;
	uint16_t dependency_offset;
	uint8_t segment_count;
	uint8_t endpoint_count;
	uint8_t dependency_count;
	uint8_t hot_index;
#if ZOS_NS_HANDLES
	uint16_t publication_offset;
	uint8_t publication_count;
#endif
	struct service_segment_info segment[ZMOD_SERVICE_MAX_SEGMENTS];
	struct service_endpoint_info endpoint[ZMOD_SERVICE_MAX_ENDPOINTS];
};

static struct service_layout service_layout;

/* The manifest name record currently read, reused for every image. */
static uint8_t service_name[ZOS_SERVICE_MANIFEST_ENTRY];

/* --- reporting (early console, written before any driver exists) --------- */

static void service_puts(const char *text)
{
	early_console_puts(text);
}

#if ZOS_SERVICE_TEST_EVIDENCE
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
#endif /* ZOS_SERVICE_TEST_EVIDENCE */

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

/* Read and bound-check one driver container's descriptors.  The packer
 * linked the image on the host (architecture 36.11): the loader checks
 * the bounds that protect the nucleus - every table inside the container,
 * every segment's final base inside its tier, every entry and dispatch
 * address inside its segment - and copies. */
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
#if ZOS_NS_HANDLES
	    /* ABI 4 carries the publication table (package 42a); this build
	     * installs it into the DEVICE provider's table below. */
	    (header[6] != ZMOD_SERVICE_ABI_VERSION &&
	     header[6] != ZMOD_SERVICE_ABI_PUBLICATIONS)
#else
	    header[6] != ZMOD_SERVICE_ABI_VERSION
#endif
	    ) {
		return ZMOD_ENOTSUP;
	}
	if (header[5] != ZMOD_TYPE_SERVICE ||
	    header[7] != ZMOD_FLAG_NONE ||
	    service_le16(header + 0x0C) != 0u ||
	    service_le16(header + 0x10) != ZMOD_NO_ENTRY ||
	    service_le16(header + 0x12) != 0u ||
	    service_le16(header + 0x14) != 0u ||
	    service_le16(header + 0x18) != source->size ||
	    service_le16(header + 0x1A) != 0u) {
		return ZMOD_EINVAL;
	}
	layout->image_size = service_le16(header + 0x08);
	layout->bss_size = service_le16(header + 0x0A);
	layout->init = service_le16(header + 0x0E);
	layout->payload_offset = service_le16(header + 0x16);
	if (layout->payload_offset < (uint16_t)(ZMOD_HEADER_SIZE +
						ZMOD_SERVICE_HEADER_SIZE) ||
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
#if ZOS_NS_HANDLES
	layout->publication_count = (header[6] ==
				     ZMOD_SERVICE_ABI_PUBLICATIONS)
		? section[0x0B] : 0u;
#endif
	layout->segment_offset = service_le16(section + 0x02);
	layout->endpoint_offset = service_le16(section + 0x04);
	layout->dependency_offset = service_le16(section + 0x06);
#if ZOS_NS_HANDLES
	layout->publication_offset = (layout->publication_count != 0u)
		? service_le16(section + 0x0C) : 0u;
#endif
	if (layout->segment_count == 0u ||
	    layout->segment_count > ZMOD_SERVICE_MAX_SEGMENTS ||
	    layout->endpoint_count > ZMOD_SERVICE_MAX_ENDPOINTS ||
	    layout->dependency_count > ZMOD_SERVICE_MAX_DEPENDENCIES
#if ZOS_NS_HANDLES
	    || layout->publication_count > ZMOD_SERVICE_MAX_PUBLICATIONS
#endif
	    ) {
		return ZMOD_EFORMAT;
	}
	/* Every table sits inside the container, below the payload. */
	if (service_table_end(layout->segment_offset,
			      ZMOD_SERVICE_SEGMENT_SIZE,
			      layout->segment_count) > layout->payload_offset ||
	    service_table_end(layout->endpoint_offset,
			      ZMOD_SERVICE_ENDPOINT_SIZE,
			      layout->endpoint_count) >
	    layout->payload_offset ||
	    service_table_end(layout->dependency_offset,
			      ZMOD_SERVICE_DEPENDENCY_SIZE,
			      layout->dependency_count) >
	    layout->payload_offset) {
		return ZMOD_EFORMAT;
	}
#if ZOS_NS_HANDLES
	if (service_table_end(layout->publication_offset,
			      ZMOD_SERVICE_PUBLICATION_SIZE,
			      layout->publication_count) >
	    layout->payload_offset) {
		return ZMOD_EFORMAT;
	}
#endif

	layout->hot_index = 0xFFu;
	for (index = 0u; index < layout->segment_count; ++index) {
		struct service_segment_info *segment = &layout->segment[index];
		uint8_t role_bit;
		uint16_t tier_base;
		uint16_t tier_end;

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
		segment->flags = record[1];
		segment->base = service_le16(record + 0x02);
		segment->image_size = service_le16(record + 0x04);
		segment->bss_size = service_le16(record + 0x06);
		segment->entry = service_le16(record + 0x08);
		segment->payload_offset = service_le16(record + 0x0A);
		segment->reserved = service_le16(record + 0x0C);
		if (segment->flags != 0u || segment->reserved != 0u) {
			return ZMOD_EINVAL;
		}
		if (segment->image_size == 0u && segment->bss_size == 0u) {
			return ZMOD_EFORMAT;
		}
		if ((uint32_t)segment->payload_offset +
		    segment->image_size > layout->image_size) {
			return ZMOD_EFORMAT;
		}
		/* The final base and the whole allocation stay in the
		 * segment's tier: this is what protects the fixed driver-bank
		 * layout and the nucleus's own windows. */
		if (segment->role == ZMOD_SEGMENT_HOT) {
			tier_base = SERVICE_HOT_BASE;
			tier_end = SERVICE_HOT_END;
			layout->hot_index = index;
		} else if (segment->role == ZMOD_SEGMENT_IO) {
			tier_base = SERVICE_IO_BASE;
			tier_end = SERVICE_IO_END;
		} else {
			tier_base = SERVICE_BOOT_BASE;
			tier_end = SERVICE_BOOT_END;
		}
		if (segment->base < tier_base ||
		    (uint32_t)segment->base + segment->image_size +
		    segment->bss_size > tier_end) {
			return ZMOD_EFORMAT;
		}
		if (segment->entry != ZMOD_NO_ENTRY &&
		    (segment->role != ZMOD_SEGMENT_BOOT ||
		     segment->entry < segment->base ||
		     segment->entry >=
		     (uint16_t)(segment->base + segment->image_size))) {
			return ZMOD_EFORMAT;
		}
	}
	if (layout->hot_index == 0xFFu) {
		return ZMOD_EFORMAT;
	}
	if (layout->init != ZMOD_NO_ENTRY &&
	    (layout->init < layout->segment[layout->hot_index].base ||
	     layout->init >=
	     (uint16_t)(layout->segment[layout->hot_index].base +
			layout->segment[layout->hot_index].image_size))) {
		return ZMOD_EFORMAT;
	}

	for (index = 0u; index < layout->endpoint_count; ++index) {
		struct service_endpoint_info *endpoint = &layout->endpoint[index];
		const struct service_segment_info *hot =
			&layout->segment[layout->hot_index];

		if (service_read(source,
				 (uint16_t)(ZMOD_SERVICE_SECTION_OFFSET +
					    layout->endpoint_offset +
					    (uint16_t)(ZMOD_SERVICE_ENDPOINT_SIZE *
						       index)),
				 record, ZMOD_SERVICE_ENDPOINT_SIZE) !=
		    ZMOD_OK) {
			return ZMOD_EFORMAT;
		}
		endpoint->class = record[0];
		endpoint->instance = record[1];
		endpoint->version = record[2];
		endpoint->flags = record[3];
		endpoint->number = service_le16(record + 0x04);
		endpoint->dispatch = service_le16(record + 0x06);
		if (endpoint->number == 0u ||
		    (endpoint->flags & (uint8_t)~ZMOD_SERVICE_EP_REENTRANT) !=
		    0u) {
			return ZMOD_EINVAL;
		}
		/* The dispatch entry must lie inside the hot image: the
		 * one address the nucleus will call in bank 6. */
		if (endpoint->dispatch < hot->base ||
		    endpoint->dispatch >=
		    (uint16_t)(hot->base + hot->image_size)) {
			return ZMOD_EINVAL;
		}
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

/* --- table installation and bring-up ------------------------------------- */

/* Install every endpoint the image declares, with the image's bulk extent,
 * into the nucleus table (the host-built table, copied in).  The nucleus
 * validates the record; a refusal is a boot error. */
static int service_install_endpoints(const struct service_layout *layout,
				     uint16_t bulk_base, uint16_t bulk_size,
				     uint8_t *number_high)
{
	uint8_t index;

	for (index = 0u; index < layout->endpoint_count; ++index) {
		const struct service_endpoint_info *endpoint =
			&layout->endpoint[index];
		struct ipc_endpoint_record record;

		record.class = endpoint->class;
		record.instance = endpoint->instance;
		record.version = endpoint->version;
		record.flags = endpoint->flags;
		record.number = (uint8_t)endpoint->number;
		record.entry = endpoint->dispatch;
		record.bulk_base = bulk_base;
		record.bulk_size = bulk_size;
		if (ipc_install_endpoint(&record) < 0) {
			service_fail(service_name, "endpoint refused");
			return ZMOD_ENOSPC;
		}
		if (endpoint->number > *number_high) {
			*number_high = (uint8_t)endpoint->number;
		}
	}
	return ZMOD_OK;
}

#if ZOS_NS_HANDLES
/* Install the image's host-built publications into the DEVICE provider's
 * table (architecture 36.13, work package 42a).  The packer resolved the
 * serving endpoint number; the loader checks the record against the table
 * that is actually installed - the name shape, the protocol type and that
 * the endpoint is one this build has - and the provider refuses anything
 * that would overflow or collide. */
static int service_install_publications(const struct zmod_source *source,
					const struct service_layout *layout)
{
	uint8_t index;

	for (index = 0u; index < layout->publication_count; ++index) {
		uint8_t record[ZMOD_SERVICE_PUBLICATION_SIZE];
		uint8_t length;
		uint8_t number;

		if (service_read(source,
				 (uint16_t)(ZMOD_SERVICE_SECTION_OFFSET +
					    layout->publication_offset +
					    ZMOD_SERVICE_PUBLICATION_SIZE *
					    index),
				 record, ZMOD_SERVICE_PUBLICATION_SIZE) !=
		    ZMOD_OK) {
			return ZMOD_EFORMAT;
		}
		length = 0u;
		while (length < (uint8_t)sizeof(((struct
						  zmod_service_publication *)
						 0)->name) &&
		       record[length] != '\0') {
			++length;
		}
		if (length == 0u ||
		    length >= (uint8_t)sizeof(((struct
						zmod_service_publication *)
					       0)->name) ||
		    record[0] != '/' || record[0x10] == 0u) {
			return ZMOD_EFORMAT;
		}
		number = record[0x11];
		if (number == 0u) {
			return ZMOD_EFORMAT;
		}
		if (vfs_ns_install_publication((const char *)record,
					       record[0x10], number,
					       record[0x12]) < 0) {
			return ZMOD_EFORMAT;
		}
	}
	return ZMOD_OK;
}

#endif

/* Install the image's resolved dependency numbers into the fixed-core
 * array the boot bundle names.  The host resolved each number against an
 * earlier (or this image's own) export; the loader checks that the number
 * it installs is one the nucleus already has, and zeroes the unused
 * tail. */
static int service_read_dependencies(const struct zmod_source *source,
				     const struct service_layout *layout,
				     uint8_t number_high)
{
	uint8_t index;

	for (index = 0u; index < ZMOD_SERVICE_MAX_DEPENDENCIES; ++index) {
		uint16_t number = 0u;

		if (index < layout->dependency_count) {
			uint8_t record[ZMOD_SERVICE_DEPENDENCY_SIZE];
			uint16_t at = (uint16_t)(ZMOD_SERVICE_SECTION_OFFSET +
				layout->dependency_offset +
				(uint16_t)(ZMOD_SERVICE_DEPENDENCY_SIZE *
					   index));

			if (service_read(source, at, record,
					 ZMOD_SERVICE_DEPENDENCY_SIZE) !=
			    ZMOD_OK) {
				return ZMOD_EFORMAT;
			}
			number = service_le16(record + 0x02);
			if (number > number_high) {
				return ZMOD_EFORMAT;
			}
		}
		zos_service_deps[index] = (uint8_t)number;
	}
	return ZMOD_OK;
}

static int service_run_boot(const struct service_layout *layout)
{
	uint8_t index;

	for (index = 0u; index < layout->segment_count; ++index) {
		if (layout->segment[index].role != ZMOD_SEGMENT_BOOT) {
			continue;
		}
		if (layout->segment[index].entry == ZMOD_NO_ENTRY) {
			continue; /* a boot/bulk data segment runs nothing */
		}
		if (zos_service_run_boot(layout->segment[index].entry) != 0u) {
			service_fail(service_name, "boot refused");
			return ZMOD_EINVAL;
		}
	}
	return ZMOD_OK;
}

/* The hot init entry, once, through the fixed upcall shim: the mode-11
 * window shows bank 6 and the fixed core stays visible, so the driver
 * reads its boot bundle and claims its IRQs here. */
static int service_run_init(const struct service_layout *layout)
{
	if (layout->init == ZMOD_NO_ENTRY) {
		return ZMOD_OK;
	}
	zos_service_upcall_block.page = ZOS_UPCALL_PAGE_SERVICE;
	zos_service_upcall_block.entry = layout->init;
	zos_service_upcall_block.args = (void *)ipc_service_boot();
	zos_service_upcall_block.result = 0u;
	if (zos_upcall(&zos_service_upcall_block) != 0u) {
		service_fail(service_name, "init refused");
		return ZMOD_EINVAL;
	}
	return ZMOD_OK;
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

/* The image's bulk extent, if it carries a bulk segment (architecture 36.6:
 * the run-time bulk copy is bound to the driver that owns the dispatched
 * endpoint). */
static void service_bulk_extent(const struct service_layout *layout,
				uint16_t *base, uint16_t *size)
{
	uint8_t index;

	*base = 0u;
	*size = 0u;
	for (index = 0u; index < layout->segment_count; ++index) {
		if (layout->segment[index].role == ZMOD_SEGMENT_BULK) {
			*base = layout->segment[index].base;
			*size = (uint16_t)(layout->segment[index].image_size +
					   layout->segment[index].bss_size);
		}
	}
}

/* The plan line the acceptance transcript pins: the roles in the order
 * tools/zmodpack.py's `services` command prints them, absent roles left
 * out.  Evidence only: production does not link it. */
#if ZOS_SERVICE_TEST_EVIDENCE
static void service_report(const struct service_layout *layout)
{
	static const uint8_t roles[4] = {
		ZMOD_SEGMENT_HOT, ZMOD_SEGMENT_IO,
		ZMOD_SEGMENT_BOOT, ZMOD_SEGMENT_BULK
	};
	static const char *const names[4] = {
		" hot ", " io ", " boot ", " bulk "
	};
	uint8_t role_index;

	service_puts("driver ");
	service_puts((const char *)service_name);
	service_puts(":");
	for (role_index = 0u; role_index < 4u; ++role_index) {
		uint8_t index;

		for (index = 0u; index < layout->segment_count; ++index) {
			if (layout->segment[index].role != roles[role_index]) {
				continue;
			}
			service_puts(names[role_index]);
			service_put_hex16(layout->segment[index].base);
			break;
		}
	}
	service_puts("\r\n");
}
#endif /* ZOS_SERVICE_TEST_EVIDENCE */

int service_load_all(const char *manifest_name)
{
	romfs_file_t manifest;
	romfs_file_t file;
	struct zmod_source source;
	uint16_t entries;
	uint16_t index;
	uint8_t number_high = 0u;
	int result;

	if (service_loader_ran) {
		return ZMOD_OK; /* drivers are permanent: load once */
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

	/* Load pass, in manifest order: copy the host-linked segments, install
	 * the host-built endpoint and dependency tables, run the boot segment
	 * once through the mode-10 gate, then the hot init entry once through
	 * the fixed upcall shim. */
	for (index = 0u; index < entries; ++index) {
		uint16_t bulk_base;
		uint16_t bulk_size;
		uint8_t segment_index;

		result = service_open(&manifest, index, &file, &source);
		if (result == ZMOD_OK) {
			result = service_read_layout(&source, &service_layout);
			if (result != ZMOD_OK) {
				service_fail(service_name, "invalid image");
			}
		}
		if (result != ZMOD_OK) {
			return result;
		}
		for (segment_index = 0u;
		     segment_index < service_layout.segment_count;
		     ++segment_index) {
			const struct service_segment_info *segment =
				&service_layout.segment[segment_index];

			result = service_place(
				&source,
				(uint16_t)(service_layout.payload_offset +
					   segment->payload_offset),
				segment->base, segment->image_size);
			if (result == ZMOD_OK && segment->bss_size != 0u &&
			    service_bank_op(ZOS_BANK_OP_ZERO,
					    (uint16_t)(segment->base +
						       segment->image_size),
					    segment->bss_size) != 0u) {
				result = ZMOD_EFORMAT;
			}
			if (result != ZMOD_OK) {
				service_fail(service_name, "copy refused");
				return result;
			}
			if (segment->role == ZMOD_SEGMENT_HOT ||
			    segment->role == ZMOD_SEGMENT_IO) {
				service_hot_ledger = (uint16_t)(
					service_hot_ledger +
					segment->image_size + segment->bss_size);
			} else {
				service_boot_ledger = (uint16_t)(
					service_boot_ledger +
					segment->image_size + segment->bss_size);
			}
		}
		service_bulk_extent(&service_layout, &bulk_base, &bulk_size);
		result = service_install_endpoints(&service_layout, bulk_base,
						   bulk_size, &number_high);
		if (result != ZMOD_OK) {
			return result;
		}
#if ZOS_NS_HANDLES
		result = service_install_publications(&source,
						      &service_layout);
		if (result != ZMOD_OK) {
			service_fail(service_name, "publication refused");
			return result;
		}
#endif
		result = service_read_dependencies(&source, &service_layout,
						   number_high);
		if (result != ZMOD_OK) {
			service_fail(service_name, "invalid image");
			return result;
		}
		result = service_run_boot(&service_layout);
		if (result == ZMOD_OK) {
			result = service_run_init(&service_layout);
		}
		if (result != ZMOD_OK) {
			return result;
		}
#if ZOS_SERVICE_TEST_EVIDENCE
		service_report(&service_layout);
#endif
	}

	service_loader_ran = 1u;
#if ZOS_SERVICE_TEST_EVIDENCE
	/* Evidence only: the ledger tail the replay pins against the
	 * packer's report. */
	service_puts("service loader: ");
	service_put_dec16(entries);
	service_puts(" images in bank 6\r\n");
	service_puts("driver hot: ");
	service_put_dec16(service_hot_ledger);
	service_puts(" bytes of 32768\r\n");
	service_puts("driver boot/bulk: ");
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
