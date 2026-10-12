/* KonaOS resident-driver loader and boot manifest reader.
 *
 * Architecture 36.6, 36.11 and 36.12 (work packages 40-42c).  This
 * cold-half nucleus loader turns the flat load plans the recovery store
 * carries into the bank-6 resident domain, in manifest order:
 *
 *   1. read the boot manifest (KOS_DRIVER_MANIFEST_NAME) with the
 *      bootstrap ROMFS reader: a flat DATA file of 24-byte ROMFS name
 *      records, the load order;
 *   2. read each image's flat plan (formats/include/mod.h) and
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
 * placement policy, and there is no magic check - the ROMFS entry's MOD
 * flag (validated by romfs_init) is what proves the file is a driver
 * image.  The loader reads each plan once and checks only the bounds that
 * protect the nucleus and the fixed driver-bank layout - the header's
 * table counts against the loader's tables, each segment's allocation
 * inside its tier, each called entry inside the region it will run in,
 * each dependency number against what the nucleus has installed, each
 * endpoint number nonzero and the dispatch entry inside the driver window
 * (ipc_install_endpoint).
 *
 * Boot-tier code runs with bank 7 unmapped (see kernel/driver/
 * driver_boot.asm): the boot entry may touch bank 6, the common region
 * and I/O ports, and it must not call the bootstrap interface.
 */

#include "mod.h"
#include "kos/errno.h"
#include "driver.h"
#include "kernel.h"
#include "process.h"
#include "romfs.h"

#include "kernel/ipc/ipc.h"
#include "kernel/memory/upcall.h"
#include "kernel/syscall/diag.h"
#include "kernel/vfs/vfs.h"

/* The plan and ledger lines are acceptance evidence the replay pins
 * against the packer's report; production links only the error lines
 * (work package 41).  Target images normally inherit the shared
 * KOS_TARGET_TEST_BUILD guard. */
#ifndef KOS_TARGET_TEST_BUILD
#define KOS_TARGET_TEST_BUILD 0
#endif
#ifndef KOS_DRIVER_TEST_EVIDENCE
#define KOS_DRIVER_TEST_EVIDENCE KOS_TARGET_TEST_BUILD
#endif

/* Bank 6 is the resident driver domain (revision 13: bank = KOS_BANK_MASK
 * - window page; the same derivation kernel/ipc/ipc.c uses). */
#define DRIVER_BANK (uint8_t)(KOS_BANK_MASK - 1u)

/* Destination tiers (architecture 36.6): the hot tier 6000h-DFFFh (the io
 * role lives inside 8000h-BFFFh, which the hot-tier bound already
 * contains), the boot/bulk tier 0000h-5FFFh.  The host computed each
 * address; these are the ranges the loader checks. */
#define DRIVER_HOT_BASE  KOS_REV13_WINDOW_BASE /* 6000h */
#define DRIVER_HOT_END   KOS_BANK7_RUNTIME_END /* E000h */
#define DRIVER_BOOT_END  KOS_REV13_WINDOW_BASE /* 6000h */

/* The boot gate (kernel/driver/driver_boot.asm) and the upcall shim's
 * fixed-core request block and entry point (kernel/memory/upcall.h). */
extern uint8_t kos_driver_run_boot(uint16_t entry);
extern struct kos_upcall_req kos_driver_upcall_block;

/* Manifest entry N's logical offset: 24 = 16 + 8, computed with shifts
 * because the kernel links no multiply runtime (kernel/romfs/romfs.c). */
#define DRIVER_MANIFEST_AT(n) \
	((uint16_t)(((uint16_t)(n) << 4) + ((uint16_t)(n) << 3)))

static uint8_t driver_loader_ran;
#if KOS_DRIVER_TEST_EVIDENCE
static uint16_t driver_hot_ledger;
static uint16_t driver_boot_ledger;
/* MOD_SEGMENT_* - 1.  The sentinel and not 0 marks an absent role: a
 * boot or bulk segment may legitimately be placed at 0000h. */
#define DRIVER_ROLE_ABSENT 0xFFFFu
static uint16_t driver_role_base[4];
static const uint8_t *driver_report_name;
#endif

/* --- reporting (early console, written before any driver exists) --------- */

static void driver_puts(const char *text)
{
	early_console_puts(text);
}

#if KOS_DRIVER_TEST_EVIDENCE
static void driver_put_hex16(uint16_t value)
{
	uint8_t shift;
	static const char digits[] = "0123456789ABCDEF";

	for (shift = 16u; shift != 0u; ) {
		shift = (uint8_t)(shift - 4u);
		early_console_putc(digits[(value >> shift) & 0x0Fu]);
	}
	early_console_putc('h');
}
#endif /* KOS_DRIVER_TEST_EVIDENCE */

#if KOS_DRIVER_TEST_EVIDENCE
/* Decimal without the SDCC division runtime (work/30b audit: the kernel
 * links no arithmetic helpers): repeated subtraction of the powers of
 * ten, which for a 16-bit value is at most 50 iterations. */
static void driver_put_dec16(uint16_t value)
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
#endif /* KOS_DRIVER_TEST_EVIDENCE */

/* One failure line: "driver loader: <name>: <what>", the boot error the
 * contract requires on the early console.  The class carries the text, so
 * every failing step only returns its code; the words are short because
 * the loader's byte budget is an architectural resource (36.3):
 * "no image" the store does not hold the manifest's file, "copy" a segment
 * that does not fit or a refused copy, "endpoint" the endpoint table is
 * full, "publication" the namespace refused the row, "invalid" a corrupt
 * or truncated container (docs/module-format.md section 7). */
static void driver_fail(const uint8_t *name, int result)
{
	driver_puts("driver loader: ");
	if (name != (const uint8_t *)0) {
		driver_puts((const char *)name);
		driver_puts(": ");
	}
	if (result == MOD_ENOENT) {
		driver_puts("no image");
	} else if (result == MOD_ENOTSUP) {
		driver_puts("copy");
	} else if (result == MOD_ENOSPC) {
		driver_puts("endpoint");
	} else if (result == MOD_EBUSY) {
		driver_puts("publication");
	} else {
		driver_puts("invalid");
	}
	driver_puts("\r\n");
}

/* --- container reads ----------------------------------------------------- */

/* Read one plan record of `size` bytes at `at`, advancing `at`.  The
 * reader bounds the file itself (romfs_read rejects a request that leaves
 * the file), so this only has to walk the tables the header declared. */
static int driver_record(const romfs_file_t *file, uint16_t *at,
			  void *record, uint8_t size)
{
	if (romfs_read(file, *at, record, size) != (int)size) {
		return MOD_EFORMAT;
	}
	*at = (uint16_t)(*at + size);
	return MOD_OK;
}

/* --- bank-6 access through the common copy gate -------------------------- */

static uint8_t driver_bank_op(uint8_t op, uint16_t addr, uint16_t length)
{
	struct kos_bank_copy_req req;

	req.bank = DRIVER_BANK;
	req.op = op;
	req.addr = addr;
	req.buffer = kos_user_copy_staging;
	req.length = length;
	return kos_bank_copy(&req);
}

/* Copy `length` container bytes to bank-6 `dst` through the common
 * staging buffer and the gate (which validates the bank and restores the
 * exact latch, stack and interrupt state). */
static int driver_place(const romfs_file_t *file, uint16_t offset,
			 uint16_t dst, uint16_t length)
{
	uint16_t done = 0u;

	while (done < length) {
		uint16_t chunk = (uint16_t)(length - done);

		if (chunk > KOS_SYSCALL_STAGE_SIZE) {
			chunk = KOS_SYSCALL_STAGE_SIZE;
		}
		if (romfs_read(file, (uint16_t)(offset + done),
			       kos_user_copy_staging, chunk) != (int)chunk) {
			return MOD_EFORMAT;
		}
		if (driver_bank_op(KOS_BANK_OP_WRITE,
				    (uint16_t)(dst + done), chunk) != 0u) {
			return MOD_EFORMAT;
		}
		done = (uint16_t)(done + chunk);
	}
	return MOD_OK;
}

/* The plan line the acceptance transcript pins: the roles in the order
 * tools/modpack.py's `drivers` command prints them, absent roles left
 * out.  Evidence only: production does not link it. */
#if KOS_DRIVER_TEST_EVIDENCE
static void driver_report(void)
{
	static const uint8_t order[4] = {
		MOD_SEGMENT_HOT, MOD_SEGMENT_IO,
		MOD_SEGMENT_BOOT, MOD_SEGMENT_BULK
	};
	static const char *const names[4] = {
		" hot ", " boot ", " io ", " bulk "
	};
	uint8_t index;

	driver_puts("driver ");
	driver_puts((const char *)driver_report_name);
	driver_puts(":");
	for (index = 0u; index < 4u; ++index) {
		uint8_t role = order[index];
		uint16_t base = driver_role_base[role - 1u];

		if (base != DRIVER_ROLE_ABSENT) {
			driver_puts(names[role - 1u]);
			driver_put_hex16(base);
		}
	}
	driver_puts("\r\n");
}
#endif /* KOS_DRIVER_TEST_EVIDENCE */

/* --- one image's flat plan ------------------------------------------------ */

/* Copy the segments, install the tables and run the entries of one image.
 * `source` is the container; every address is host-final, so the checks
 * here are the ones that keep a bad plan out of the nucleus: the tier
 * bounds, the boot/init entry bounds, the endpoint numbers and the
 * dependency numbers.  A failure returns a loader error code and the one
 * caller prints the line. */
static int driver_load_image(const romfs_file_t *file,
			      const struct mod_header *header)
{
	struct mod_segment segment;
	uint16_t at = MOD_HEADER_SIZE;
	uint16_t boot_entry = MOD_NO_ENTRY;
	uint16_t hot_base = 0u;
	uint16_t hot_image = 0u;
	uint8_t number_high = 0u;
	uint8_t index;
	uint8_t count = header->segment_count;
	uint16_t bulk_base = 0u;
	uint16_t bulk_size = 0u;

#if KOS_DRIVER_TEST_EVIDENCE
	for (index = 0u; index < 4u; ++index) {
		driver_role_base[index] = DRIVER_ROLE_ABSENT;
	}
#endif

	/* Segment pass: place before installing, so a segment that fails
	 * its bounds costs no table state. */
	for (index = 0u; index < count; ++index) {
		uint16_t end;
		uint16_t payload_at;

		if (driver_record(file, &at, &segment,
				   MOD_SEGMENT_SIZE) != MOD_OK) {
			return MOD_EFORMAT;
		}
		if (segment.role == 0u || segment.role > MOD_SEGMENT_BULK) {
			return MOD_EINVAL;
		}
		/* The whole allocation must sit in the segment's tier: this
		 * is what protects the fixed driver-bank layout.  The sum
		 * wrapping (end < base) would put it outside every tier. */
		end = (uint16_t)(segment.base + segment.image_size +
				 segment.bss_size);
		if (end < segment.base ||
		    end > ((segment.base >= DRIVER_HOT_BASE)
			   ? DRIVER_HOT_END : DRIVER_BOOT_END)) {
			return MOD_EFORMAT;
		}
		if (segment.role == MOD_SEGMENT_HOT) {
			hot_base = segment.base;
			hot_image = segment.image_size;
		}
		if (segment.role == MOD_SEGMENT_BULK) {
			bulk_base = segment.base;
			bulk_size = (uint16_t)(segment.image_size +
					       segment.bss_size);
		}
		if (segment.entry != MOD_NO_ENTRY) {
			/* Only the boot role carries one, and it runs with
			 * the whole bank 6 visible (mode 10, Q = 6). */
			if (segment.role != MOD_SEGMENT_BOOT ||
			    segment.entry >= DRIVER_BOOT_END) {
				return MOD_EINVAL;
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
		    driver_place(file, payload_at, segment.base,
				  segment.image_size) != MOD_OK) {
			return MOD_ENOTSUP;
		}
		if (segment.bss_size != 0u &&
		    driver_bank_op(KOS_BANK_OP_ZERO,
				    (uint16_t)(segment.base +
					       segment.image_size),
				    segment.bss_size) != 0u) {
			return MOD_ENOTSUP;
		}
#if KOS_DRIVER_TEST_EVIDENCE
		if (segment.role == MOD_SEGMENT_HOT ||
		    segment.role == MOD_SEGMENT_IO) {
			driver_hot_ledger = (uint16_t)(
				driver_hot_ledger + segment.image_size +
				segment.bss_size);
		} else {
			driver_boot_ledger = (uint16_t)(
				driver_boot_ledger + segment.image_size +
				segment.bss_size);
		}
		driver_role_base[segment.role - 1u] = segment.base;
#endif
	}
	if (hot_image == 0u ||
	    (header->init_entry != MOD_NO_ENTRY &&
	     (header->init_entry < hot_base ||
	      header->init_entry >= (uint16_t)(hot_base + hot_image)))) {
		return MOD_EINVAL;
	}

	/* Endpoint table: ipc_install_endpoint validates the dispatch entry
	 * and the table shape; the loader refuses number 0 (the nucleus
	 * reads 0 as "no endpoint"). */
	for (index = 0u, count = header->endpoint_count; index < count;
	     ++index) {
		struct mod_endpoint endpoint;
		struct ipc_endpoint_record record;

		if (driver_record(file, &at, &endpoint,
				   MOD_ENDPOINT_SIZE) != MOD_OK) {
			return MOD_EFORMAT;
		}
		if (endpoint.number == 0u) {
			return MOD_EINVAL;
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
			return MOD_ENOSPC;
		}
		if (endpoint.number > number_high) {
			number_high = (uint8_t)endpoint.number;
		}
	}

	/* Dependency table: the host resolved each declaration to an
	 * endpoint number the nucleus already has, and zeroed the unused
	 * tail so the init always reads a full array. */
	for (index = 0u; index < MOD_MAX_DEPENDENCIES; ++index) {
		uint16_t number = 0u;

		if (index < header->dependency_count) {
			struct mod_dependency dependency;

			if (driver_record(file, &at, &dependency,
					   MOD_DEPENDENCY_SIZE) != MOD_OK) {
				return MOD_EFORMAT;
			}
			number = dependency.number;
			if (number > number_high) {
				return MOD_EFORMAT;
			}
		}
		kos_driver_deps[index] = (uint8_t)number;
	}

	/* Publication table: the DEVICE and FS providers validate the name,
	 * the type and the number; the loader only keeps the provider from
	 * scanning past the record. */
	for (index = 0u, count = header->publication_count; index < count;
	     ++index) {
		struct mod_publication publication;

		if (driver_record(file, &at, &publication,
				   MOD_PUBLICATION_SIZE) != MOD_OK) {
			return MOD_EFORMAT;
		}
		if (publication.name[15] != '\0' ||
		    vfs_ns_install_publication(publication.name,
					       publication.type,
					       publication.number,
					       publication.object) < 0) {
			return MOD_EBUSY;
		}
	}

	/* The boot segment once, through the mode-10 gate, then the hot
	 * init once, through the fixed upcall shim. */
	if (boot_entry != MOD_NO_ENTRY &&
	    kos_driver_run_boot(boot_entry) != 0u) {
		return MOD_EINVAL;
	}
	if (header->init_entry != MOD_NO_ENTRY) {
		/* The mode-11 window shows bank 6 and the fixed core stays
		 * visible, so the driver reads its boot bundle and claims
		 * its IRQs here.  The init's uint8_t return is published by
		 * the upcall shim; a non-zero status fails the image and
		 * the boot (36.11). */
		kos_driver_upcall_block.page = KOS_UPCALL_PAGE_DRIVER;
		kos_driver_upcall_block.entry = header->init_entry;
		kos_driver_upcall_block.args = (void *)driver_boot();
		if (kos_upcall(&kos_driver_upcall_block) != 0u ||
		    kos_upcall_ret_a != 0u) {
			return MOD_EINVAL;
		}
	}
#if KOS_DRIVER_TEST_EVIDENCE
	driver_report();
#endif
	return MOD_OK;
}

/* --- the loader ---------------------------------------------------------- */

/* Read and bound one image's flat plan header: it must hold the header,
 * the table counts must fit the loader's tables, and the payload must
 * start inside the image.  There is no magic and no version check: the
 * ROMFS entry's MOD flag (validated by romfs_init) is what proves the
 * file is a driver image, and the packer is its only writer.  The
 * endpoint and publication counts are not bounded here: the nucleus
 * tables (ipc_install_endpoint, vfs_ns_install_publication) refuse an
 * overflow themselves. */
static int driver_read_header(const romfs_file_t *file, uint16_t size,
			       struct mod_header *header)
{
	if (size < MOD_HEADER_SIZE ||
	    romfs_read(file, 0u, header,
		       MOD_HEADER_SIZE) != (int)MOD_HEADER_SIZE) {
		return MOD_EFORMAT;
	}
	if (header->segment_count > MOD_MAX_SEGMENTS ||
	    header->dependency_count > MOD_MAX_DEPENDENCIES ||
	    header->total_size != size ||
	    header->payload_offset > size) {
		return MOD_EFORMAT;
	}
	return MOD_OK;
}

int driver_load_all(const char *manifest_name)
{
	romfs_file_t manifest;
	romfs_file_t file;
	struct mod_header header;
	uint8_t name[KOS_DRIVER_MANIFEST_ENTRY];
	uint16_t size;
	uint16_t entries;
	uint16_t index;
	int result;

	if (driver_loader_ran) {
		return MOD_OK; /* drivers are permanent: load once */
	}
	if (manifest_name == (const char *)0 ||
	    romfs_lookup_ci(manifest_name, &manifest) != 0) {
		driver_fail((const uint8_t *)0, MOD_EFORMAT);
		return MOD_EFORMAT;
	}
	if (manifest.flags != ROMFS_FLAG_DATA ||
	    manifest.size == 0u ||
	    manifest.size > (uint32_t)(KOS_DRIVER_MANIFEST_ENTRY *
				       MOD_MAX_IMAGES)) {
		driver_fail((const uint8_t *)0, MOD_EFORMAT);
		return MOD_EFORMAT;
	}
	/* Entry count by repeated subtraction: the kernel links no SDCC
	 * division helpers (work/30b audit), so the count is taken from the
	 * size and the manifest must be whole records. */
	entries = 0u;
	{
		uint16_t remaining = (uint16_t)manifest.size;

		while (remaining >= KOS_DRIVER_MANIFEST_ENTRY) {
			remaining = (uint16_t)(remaining -
					       KOS_DRIVER_MANIFEST_ENTRY);
			++entries;
		}
		if (remaining != 0u) {
			driver_fail((const uint8_t *)0, MOD_EFORMAT);
			return MOD_EFORMAT;
		}
	}

	/* Load pass, in manifest order: resolve the name record in the
	 * recovery store, read the image's flat plan, then copy the
	 * host-linked segments, install the host-built tables and run the
	 * boot and init entries. */
	for (index = 0u; index < entries; ++index) {
		if (romfs_read(&manifest, DRIVER_MANIFEST_AT(index), name,
			       KOS_DRIVER_MANIFEST_ENTRY) !=
		    (int)KOS_DRIVER_MANIFEST_ENTRY) {
			driver_fail((const uint8_t *)0, MOD_EFORMAT);
			return MOD_EFORMAT;
		}
		/* The name field is fixed-width; the ROMFS reader and the
		 * packer both terminate it, so this only keeps the print
		 * inside it. */
		name[KOS_DRIVER_MANIFEST_ENTRY - 1u] = '\0';
		if (romfs_lookup_ci((const char *)name, &file) != 0 ||
		    file.flags != ROMFS_FLAG_MOD ||
		    file.size > 0xFFFFu) {
			driver_fail(name, MOD_ENOENT);
			return MOD_ENOENT;
		}
		size = (uint16_t)file.size;
#if KOS_DRIVER_TEST_EVIDENCE
		driver_report_name = name;
#endif
		result = driver_read_header(&file, size, &header);
		if (result == MOD_OK) {
			result = driver_load_image(&file, &header);
		}
		if (result != MOD_OK) {
			driver_fail(name, result);
			return result;
		}
	}

	driver_loader_ran = 1u;
#if KOS_DRIVER_TEST_EVIDENCE
	/* Evidence only: the ledger tail the replay pins against the
	 * packer's report. */
	driver_puts("driver loader: ");
	driver_put_dec16(entries);
	driver_puts(" images in bank 6\r\n");
	driver_puts("driver hot: ");
	driver_put_dec16(driver_hot_ledger);
	driver_puts(" bytes of 32768\r\n");
	driver_puts("driver boot/bulk: ");
	driver_put_dec16(driver_boot_ledger);
	driver_puts(" bytes of 24576\r\n");
#endif
	return MOD_OK;
}
