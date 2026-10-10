/* ZephyrOS resident-driver loader and boot manifest reader.
 *
 * Architecture 36.6, 36.11 and 36.12 (work packages 40-42c).  This
 * cold-half nucleus loader turns the flat load plans the recovery store
 * carries into the bank-6 resident domain, in manifest order:
 *
 *   1. read the boot manifest (ZOS_SERVICE_MANIFEST_NAME) with the
 *      bootstrap ROMFS reader: a flat DATA file of 24-byte ROMFS name
 *      records, the load order;
 *   2. read each image's flat plan (include/zephyros/service.h) and
 *      iterate it: copy every segment image to its host-assigned base and
 *      zero its BSS through the common application-bank copy gate,
 *      install the host-built endpoint records into the nucleus endpoint
 *      table, install the publications and the resolved dependency
 *      numbers, run the boot entry once through the mode-10 gate with
 *      Q = 6, and run the hot init entry once through the fixed upcall
 *      shim with the boot bundle (interface table and dependency array)
 *      in HL.  A non-zero init status (the entrant's uint8_t return,
 *      published by the shim) refuses the image: a required driver that
 *      fails to initialize is a boot failure (36.11);
 *   3. keep the two architecture-36.20 ledgers in acceptance builds,
 *      `driver hot` (hot plus io bytes) and `driver boot/bulk` (boot plus
 *      bulk bytes); production takes them from the packer's report.
 *
 * The host computed everything (M10-REFERENCE decision 2): the container
 * carries no version, no module type, no relocation records and no
 * placement policy, and there is no magic check - the ROMFS entry's ZMOD
 * flag (validated by romfs_init) is what proves the file is a service
 * image.  The loader reads each plan once and checks only the bounds that
 * protect the nucleus and the fixed driver-bank layout - the header's
 * table counts against the loader's tables, each segment's allocation
 * inside its tier, each called entry inside the region it will run in,
 * each dependency number against what the nucleus has installed, each
 * endpoint number nonzero and the dispatch entry inside the driver window
 * (ipc_install_endpoint).
 *
 * Boot-tier code runs with bank 7 unmapped (see kernel/service/
 * service_boot.asm): the boot entry may touch bank 6, the common region
 * and I/O ports, and it must not call the bootstrap interface.
 */

#include "zephyros/service.h"
#include "zephyros/errno.h"
#include "zephyros/ipc.h"
#include "zephyros/kernel.h"
#include "zephyros/process.h"
#include "zephyros/romfs.h"

#include "kernel/ipc/ipc.h"
#include "kernel/memory/upcall.h"
#include "kernel/syscall/diag.h"
#include "kernel/vfs/ns.h"

/* The plan and ledger lines are acceptance evidence the replay pins
 * against the packer's report; production links only the error lines
 * (work package 41).  Target images normally inherit the shared
 * ZOS_TARGET_TEST_BUILD guard. */
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
 * role lives inside 8000h-BFFFh, which the hot-tier bound already
 * contains), the boot/bulk tier 0000h-5FFFh.  The host computed each
 * address; these are the ranges the loader checks. */
#define SERVICE_HOT_BASE  ZOS_REV13_WINDOW_BASE /* 6000h */
#define SERVICE_HOT_END   ZOS_BANK7_RUNTIME_END /* E000h */
#define SERVICE_BOOT_END  ZOS_REV13_WINDOW_BASE /* 6000h */

/* The boot gate (kernel/service/service_boot.asm) and the upcall shim's
 * fixed-core request block and entry point (kernel/memory/upcall.h). */
extern uint8_t zos_service_run_boot(uint16_t entry);
extern struct zos_upcall_req zos_service_upcall_block;

/* Manifest entry N's logical offset: 24 = 16 + 8, computed with shifts
 * because the kernel links no multiply runtime (kernel/romfs/romfs.c). */
#define SERVICE_MANIFEST_AT(n) \
	((uint16_t)(((uint16_t)(n) << 4) + ((uint16_t)(n) << 3)))

static uint8_t service_loader_ran;
#if ZOS_SERVICE_TEST_EVIDENCE
static uint16_t service_hot_ledger;
static uint16_t service_boot_ledger;
/* ZMOD_SEGMENT_* - 1.  The sentinel and not 0 marks an absent role: a
 * boot or bulk segment may legitimately be placed at 0000h. */
#define SERVICE_ROLE_ABSENT 0xFFFFu
static uint16_t service_role_base[4];
static const uint8_t *service_report_name;
#endif

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

#if ZOS_SERVICE_TEST_EVIDENCE
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
#endif /* ZOS_SERVICE_TEST_EVIDENCE */

/* One failure line: "service loader: <name>: <what>", the boot error the
 * contract requires on the early console.  The class carries the text, so
 * every failing step only returns its code; the words are short because
 * the loader's byte budget is an architectural resource (36.3):
 * "no image" the store does not hold the manifest's file, "copy" a segment
 * that does not fit or a refused copy, "endpoint" the endpoint table is
 * full, "publication" the namespace refused the row, "invalid" a corrupt
 * or truncated container (docs/module-format.md section 7). */
static void service_fail(const uint8_t *name, int result)
{
	service_puts("service loader: ");
	if (name != (const uint8_t *)0) {
		service_puts((const char *)name);
		service_puts(": ");
	}
	if (result == ZMOD_ENOENT) {
		service_puts("no image");
	} else if (result == ZMOD_ENOTSUP) {
		service_puts("copy");
	} else if (result == ZMOD_ENOSPC) {
		service_puts("endpoint");
	} else if (result == ZMOD_EBUSY) {
		service_puts("publication");
	} else {
		service_puts("invalid");
	}
	service_puts("\r\n");
}

/* --- container reads ----------------------------------------------------- */

/* Read one plan record of `size` bytes at `at`, advancing `at`.  The
 * reader bounds the file itself (romfs_read rejects a request that leaves
 * the file), so this only has to walk the tables the header declared. */
static int service_record(const romfs_file_t *file, uint16_t *at,
			  void *record, uint8_t size)
{
	if (romfs_read(file, *at, record, size) != (int)size) {
		return ZMOD_EFORMAT;
	}
	*at = (uint16_t)(*at + size);
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
static int service_place(const romfs_file_t *file, uint16_t offset,
			 uint16_t dst, uint16_t length)
{
	uint16_t done = 0u;

	while (done < length) {
		uint16_t chunk = (uint16_t)(length - done);

		if (chunk > ZOS_SYSCALL_STAGE_SIZE) {
			chunk = ZOS_SYSCALL_STAGE_SIZE;
		}
		if (romfs_read(file, (uint16_t)(offset + done),
			       zos_user_copy_staging, chunk) != (int)chunk) {
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

/* The plan line the acceptance transcript pins: the roles in the order
 * tools/zmodpack.py's `services` command prints them, absent roles left
 * out.  Evidence only: production does not link it. */
#if ZOS_SERVICE_TEST_EVIDENCE
static void service_report(void)
{
	static const uint8_t order[4] = {
		ZMOD_SEGMENT_HOT, ZMOD_SEGMENT_IO,
		ZMOD_SEGMENT_BOOT, ZMOD_SEGMENT_BULK
	};
	static const char *const names[4] = {
		" hot ", " boot ", " io ", " bulk "
	};
	uint8_t index;

	service_puts("driver ");
	service_puts((const char *)service_report_name);
	service_puts(":");
	for (index = 0u; index < 4u; ++index) {
		uint8_t role = order[index];
		uint16_t base = service_role_base[role - 1u];

		if (base != SERVICE_ROLE_ABSENT) {
			service_puts(names[role - 1u]);
			service_put_hex16(base);
		}
	}
	service_puts("\r\n");
}
#endif /* ZOS_SERVICE_TEST_EVIDENCE */

/* --- one image's flat plan ------------------------------------------------ */

/* Copy the segments, install the tables and run the entries of one image.
 * `source` is the container; every address is host-final, so the checks
 * here are the ones that keep a bad plan out of the nucleus: the tier
 * bounds, the boot/init entry bounds, the endpoint numbers and the
 * dependency numbers.  A failure returns a loader error code and the one
 * caller prints the line. */
static int service_load_image(const romfs_file_t *file,
			      const struct zmod_header *header)
{
	struct zmod_segment segment;
	uint16_t at = ZMOD_HEADER_SIZE;
	uint16_t boot_entry = ZMOD_NO_ENTRY;
	uint16_t hot_base = 0u;
	uint16_t hot_image = 0u;
	uint8_t number_high = 0u;
	uint8_t index;
	uint8_t count = header->segment_count;
	uint16_t bulk_base = 0u;
	uint16_t bulk_size = 0u;

#if ZOS_SERVICE_TEST_EVIDENCE
	for (index = 0u; index < 4u; ++index) {
		service_role_base[index] = SERVICE_ROLE_ABSENT;
	}
#endif

	/* Segment pass: place before installing, so a segment that fails
	 * its bounds costs no table state. */
	for (index = 0u; index < count; ++index) {
		uint16_t end;
		uint16_t payload_at;

		if (service_record(file, &at, &segment,
				   ZMOD_SEGMENT_SIZE) != ZMOD_OK) {
			return ZMOD_EFORMAT;
		}
		if (segment.role == 0u || segment.role > ZMOD_SEGMENT_BULK) {
			return ZMOD_EINVAL;
		}
		/* The whole allocation must sit in the segment's tier: this
		 * is what protects the fixed driver-bank layout.  The sum
		 * wrapping (end < base) would put it outside every tier. */
		end = (uint16_t)(segment.base + segment.image_size +
				 segment.bss_size);
		if (end < segment.base ||
		    end > ((segment.base >= SERVICE_HOT_BASE)
			   ? SERVICE_HOT_END : SERVICE_BOOT_END)) {
			return ZMOD_EFORMAT;
		}
		if (segment.role == ZMOD_SEGMENT_HOT) {
			hot_base = segment.base;
			hot_image = segment.image_size;
		}
		if (segment.role == ZMOD_SEGMENT_BULK) {
			bulk_base = segment.base;
			bulk_size = (uint16_t)(segment.image_size +
					       segment.bss_size);
		}
		if (segment.entry != ZMOD_NO_ENTRY) {
			/* Only the boot role carries one, and it runs with
			 * the whole bank 6 visible (mode 10, Q = 6). */
			if (segment.role != ZMOD_SEGMENT_BOOT ||
			    segment.entry >= SERVICE_BOOT_END) {
				return ZMOD_EINVAL;
			}
			boot_entry = segment.entry;
		}
		/* The copy walks the container with 16-bit offsets: refuse
		 * a payload range that wraps (the reader refuses a range
		 * that leaves the file). */
		payload_at = (uint16_t)(header->payload_offset +
					segment.payload_offset);
		if (payload_at < header->payload_offset ||
		    (uint16_t)(payload_at + segment.image_size) < payload_at ||
		    service_place(file, payload_at, segment.base,
				  segment.image_size) != ZMOD_OK) {
			return ZMOD_ENOTSUP;
		}
		if (segment.bss_size != 0u &&
		    service_bank_op(ZOS_BANK_OP_ZERO,
				    (uint16_t)(segment.base +
					       segment.image_size),
				    segment.bss_size) != 0u) {
			return ZMOD_ENOTSUP;
		}
#if ZOS_SERVICE_TEST_EVIDENCE
		if (segment.role == ZMOD_SEGMENT_HOT ||
		    segment.role == ZMOD_SEGMENT_IO) {
			service_hot_ledger = (uint16_t)(
				service_hot_ledger + segment.image_size +
				segment.bss_size);
		} else {
			service_boot_ledger = (uint16_t)(
				service_boot_ledger + segment.image_size +
				segment.bss_size);
		}
		service_role_base[segment.role - 1u] = segment.base;
#endif
	}
	if (hot_image == 0u ||
	    (header->init_entry != ZMOD_NO_ENTRY &&
	     (header->init_entry < hot_base ||
	      header->init_entry >= (uint16_t)(hot_base + hot_image)))) {
		return ZMOD_EINVAL;
	}

	/* Endpoint table: ipc_install_endpoint validates the dispatch entry
	 * and the table shape; the loader refuses number 0 (the nucleus
	 * reads 0 as "no endpoint"). */
	for (index = 0u, count = header->endpoint_count; index < count;
	     ++index) {
		struct zmod_endpoint endpoint;
		struct ipc_endpoint_record record;

		if (service_record(file, &at, &endpoint,
				   ZMOD_ENDPOINT_SIZE) != ZMOD_OK) {
			return ZMOD_EFORMAT;
		}
		if (endpoint.number == 0u) {
			return ZMOD_EINVAL;
		}
		record.class = endpoint.class;
		record.instance = endpoint.instance;
		record.version = endpoint.version;
		record.flags = endpoint.flags;
		record.number = (uint8_t)endpoint.number;
		record.entry = endpoint.dispatch;
		record.bulk_base = bulk_base;
		record.bulk_size = bulk_size;
		if (ipc_install_endpoint(&record) < 0) {
			return ZMOD_ENOSPC;
		}
		if (endpoint.number > number_high) {
			number_high = (uint8_t)endpoint.number;
		}
	}

	/* Dependency table: the host resolved each declaration to an
	 * endpoint number the nucleus already has, and zeroed the unused
	 * tail so the init always reads a full array. */
	for (index = 0u; index < ZMOD_MAX_DEPENDENCIES; ++index) {
		uint16_t number = 0u;

		if (index < header->dependency_count) {
			struct zmod_dependency dependency;

			if (service_record(file, &at, &dependency,
					   ZMOD_DEPENDENCY_SIZE) != ZMOD_OK) {
				return ZMOD_EFORMAT;
			}
			number = dependency.number;
			if (number > number_high) {
				return ZMOD_EFORMAT;
			}
		}
		zos_service_deps[index] = (uint8_t)number;
	}

	/* Publication table: the DEVICE and FS providers validate the name,
	 * the type and the number; the loader only keeps the provider from
	 * scanning past the record. */
	for (index = 0u, count = header->publication_count; index < count;
	     ++index) {
		struct zmod_publication publication;

		if (service_record(file, &at, &publication,
				   ZMOD_PUBLICATION_SIZE) != ZMOD_OK) {
			return ZMOD_EFORMAT;
		}
		if (publication.name[15] != '\0' ||
		    vfs_ns_install_publication(publication.name,
					       publication.type,
					       publication.number,
					       publication.object) < 0) {
			return ZMOD_EBUSY;
		}
	}

	/* The boot segment once, through the mode-10 gate, then the hot
	 * init once, through the fixed upcall shim. */
	if (boot_entry != ZMOD_NO_ENTRY &&
	    zos_service_run_boot(boot_entry) != 0u) {
		return ZMOD_EINVAL;
	}
	if (header->init_entry != ZMOD_NO_ENTRY) {
		/* The mode-11 window shows bank 6 and the fixed core stays
		 * visible, so the driver reads its boot bundle and claims
		 * its IRQs here.  The init's uint8_t return is published by
		 * the upcall shim; a non-zero status fails the image and
		 * the boot (36.11). */
		zos_service_upcall_block.page = ZOS_UPCALL_PAGE_SERVICE;
		zos_service_upcall_block.entry = header->init_entry;
		zos_service_upcall_block.args = (void *)ipc_service_boot();
		if (zos_upcall(&zos_service_upcall_block) != 0u ||
		    zos_upcall_ret_a != 0u) {
			return ZMOD_EINVAL;
		}
	}
#if ZOS_SERVICE_TEST_EVIDENCE
	service_report();
#endif
	return ZMOD_OK;
}

/* --- the loader ---------------------------------------------------------- */

/* Read and bound one image's flat plan header: it must hold the header,
 * the table counts must fit the loader's tables, and the payload must
 * start inside the image.  There is no magic and no version check: the
 * ROMFS entry's ZMOD flag (validated by romfs_init) is what proves the
 * file is a service image, and the packer is its only writer.  The
 * endpoint and publication counts are not bounded here: the nucleus
 * tables (ipc_install_endpoint, vfs_ns_install_publication) refuse an
 * overflow themselves. */
static int service_read_header(const romfs_file_t *file, uint16_t size,
			       struct zmod_header *header)
{
	if (size < ZMOD_HEADER_SIZE ||
	    romfs_read(file, 0u, header,
		       ZMOD_HEADER_SIZE) != (int)ZMOD_HEADER_SIZE) {
		return ZMOD_EFORMAT;
	}
	if (header->segment_count > ZMOD_MAX_SEGMENTS ||
	    header->dependency_count > ZMOD_MAX_DEPENDENCIES ||
	    header->total_size != size ||
	    header->payload_offset > size) {
		return ZMOD_EFORMAT;
	}
	return ZMOD_OK;
}

int service_load_all(const char *manifest_name)
{
	romfs_file_t manifest;
	romfs_file_t file;
	struct zmod_header header;
	uint8_t name[ZOS_SERVICE_MANIFEST_ENTRY];
	uint16_t size;
	uint16_t entries;
	uint16_t index;
	int result;

	if (service_loader_ran) {
		return ZMOD_OK; /* drivers are permanent: load once */
	}
	if (manifest_name == (const char *)0 ||
	    romfs_lookup_ci(manifest_name, &manifest) != 0) {
		service_fail((const uint8_t *)0, ZMOD_EFORMAT);
		return ZMOD_EFORMAT;
	}
	if (manifest.flags != ROMFS_FLAG_DATA ||
	    manifest.size == 0u ||
	    manifest.size > (uint32_t)(ZOS_SERVICE_MANIFEST_ENTRY *
				       ZMOD_MAX_IMAGES)) {
		service_fail((const uint8_t *)0, ZMOD_EFORMAT);
		return ZMOD_EFORMAT;
	}
	/* Entry count by repeated subtraction: the kernel links no SDCC
	 * division helpers (work/30b audit), so the count is taken from the
	 * size and the manifest must be whole records. */
	entries = 0u;
	{
		uint16_t remaining = (uint16_t)manifest.size;

		while (remaining >= ZOS_SERVICE_MANIFEST_ENTRY) {
			remaining = (uint16_t)(remaining -
					       ZOS_SERVICE_MANIFEST_ENTRY);
			++entries;
		}
		if (remaining != 0u) {
			service_fail((const uint8_t *)0, ZMOD_EFORMAT);
			return ZMOD_EFORMAT;
		}
	}

	/* Load pass, in manifest order: resolve the name record in the
	 * recovery store, read the image's flat plan, then copy the
	 * host-linked segments, install the host-built tables and run the
	 * boot and init entries. */
	for (index = 0u; index < entries; ++index) {
		if (romfs_read(&manifest, SERVICE_MANIFEST_AT(index), name,
			       ZOS_SERVICE_MANIFEST_ENTRY) !=
		    (int)ZOS_SERVICE_MANIFEST_ENTRY) {
			service_fail((const uint8_t *)0, ZMOD_EFORMAT);
			return ZMOD_EFORMAT;
		}
		/* The name field is fixed-width; the ROMFS reader and the
		 * packer both terminate it, so this only keeps the print
		 * inside it. */
		name[ZOS_SERVICE_MANIFEST_ENTRY - 1u] = '\0';
		if (romfs_lookup_ci((const char *)name, &file) != 0 ||
		    file.flags != ROMFS_FLAG_ZMOD ||
		    file.size > 0xFFFFu) {
			service_fail(name, ZMOD_ENOENT);
			return ZMOD_ENOENT;
		}
		size = (uint16_t)file.size;
#if ZOS_SERVICE_TEST_EVIDENCE
		service_report_name = name;
#endif
		result = service_read_header(&file, size, &header);
		if (result == ZMOD_OK) {
			result = service_load_image(&file, &header);
		}
		if (result != ZMOD_OK) {
			service_fail(name, result);
			return result;
		}
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
