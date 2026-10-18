/* ColecoGo for KonaOS.
 *
 * All file handling, validation, BIOS/cartridge adaptation and target-bank
 * construction are performed through the public KonaOS SDK.  The final
 * handoff is necessarily a small assembly routine: it disables the hardware
 * owned by KonaOS, changes the memory latch, completes the flat 64 KiB image
 * and jumps to the ColecoVision BIOS.  It never returns.
 */

#include "kos.h"
#include "diag.h" /* SDK diagnostic wrappers (not part of kos.h) */

#define TARGET_BANK             5u
#define BIOS_BYTES              0x2000u
#define CART_BYTES              0x8000u
#define CART_BASE               0x8000u
#define CART_HALF_BYTES         0x4000u
#define TARGET_UPPER_STAGE      0x2000u
#define TARGET_STAGE_B          0x5F00u
#define TARGET_STAGE_B_BYTES    0x0100u
#define TARGET_STUB_BACKUP      0x7A00u
#define TARGET_STUB_BACKUP_MAX  0x0300u
#define TARGET_BIOS_PAGE        0x7D00u
#define TARGET_CART_TAIL        0x7E00u
#define TARGET_CART_LOWER       0x8000u
#define PAGE_BYTES              0x0100u
#define TARGET_UPPER_PREFIX     0x3F00u

#define SCAN_BITMAP_BYTES       0x1000u
#define SCAN_WORK_ITEMS         1024u
#define PAT_HEADER_BYTES        10u
#define PAT_ENTRY_BYTES         4u
#define PAT_MAX_ENTRIES         512u
#define PAT_MAX_BYTES           (17u * 128u) /* CGP1 payload plus CP/M padding */

#define VDP_BLOCK_LO            0xA0u
#define VDP_BLOCK_HI            0xBFu
#define SOUND_BLOCK_LO          0xE0u
#define ZEPHYR_VDP_DATA         0xA0u
#define ZEPHYR_VDP_COMMAND      0xA1u
#define ZEPHYR_SOUND_PORT       0xE0u

struct scan_report {
	uint16_t vdp;
	uint16_t sound;
	uint16_t indirect;
	uint8_t scanned;
	uint8_t overflow;
};

struct bios_patch {
	uint16_t offset;
	uint8_t expected;
	uint8_t replacement;
};

union work_area {
	struct {
		uint8_t bitmap[SCAN_BITMAP_BYTES];
		uint16_t work[SCAN_WORK_ITEMS];
	} scan;
	uint8_t bios[BIOS_BYTES];
	uint8_t manifest[PAT_MAX_BYTES];
};

static uint8_t cart[CART_BYTES];
static union work_area work_area;
static uint16_t scan_work_count;

extern uint8_t handoff_begin[];
extern uint8_t handoff_end[];
extern uint8_t stage_b_begin[];
extern uint8_t stage_b_end[];
extern void handoff(void);

/* Standard ColecoVision BIOS (CRC32 3AA93EF3).  Offsets name operand bytes. */
static const struct bios_patch bios_patches[] = {
	{0x0173u, 0xFFu, 0xE0u}, {0x017Cu, 0xFFu, 0xE0u},
	{0x018Eu, 0xFFu, 0xE0u}, {0x023Eu, 0xFFu, 0xE0u},
	{0x0242u, 0xFFu, 0xE0u}, {0x0246u, 0xFFu, 0xE0u},
	{0x024Au, 0xFFu, 0xE0u}, {0x0336u, 0xFFu, 0xE0u},
	{0x0355u, 0xFFu, 0xE0u}, {0x1988u, 0x0Fu, 0x07u},
	{0x18D7u, 0xBFu, 0xA1u}, {0x18DCu, 0xBFu, 0xA1u},
	{0x18DFu, 0xBEu, 0xA0u}, {0x1C93u, 0xBFu, 0xA1u},
	{0x1C98u, 0xBFu, 0xA1u}, {0x1CABu, 0xBEu, 0xA0u},
	{0x1CCCu, 0xBFu, 0xA1u}, {0x1CD1u, 0xBFu, 0xA1u},
	{0x1D0Au, 0xBFu, 0xA1u}, {0x1D0Du, 0xBFu, 0xA1u},
	{0x1D12u, 0xBEu, 0xA0u}, {0x1D40u, 0xBFu, 0xA1u},
	{0x1D43u, 0xBFu, 0xA1u}, {0x1D47u, 0xBEu, 0xA0u},
	{0x1D58u, 0xBFu, 0xA1u}
};

static uint16_t get_le16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int fail(const char *message, int detail)
{
	if (detail < 0) {
		printf("ColecoGo: %s (%d)\r\n", message, detail);
	} else {
		printf("ColecoGo: %s\r\n", message);
	}
	return 1;
}

static int file_size(const char *path, uint32_t *size)
{
	struct kfs_stat record;
	int result = stat(path, &record);

	if (result < 0) {
		return result;
	}
	if (record.type != KFS_TYPE_FILE) {
		return -KOS_EINVAL;
	}
	*size = record.size;
	return 0;
}

static int load_file(const char *path, uint8_t *buffer, uint16_t length)
{
	int handle = open(path, KFS_O_READ);
	uint16_t done = 0u;
	int result;

	if (handle < 0) {
		return handle;
	}
	while (done < length) {
		uint16_t chunk = (uint16_t)(length - done);

		/* One KonaOS IPC descriptor carries at most 4096 bytes.  Asking the
		 * FILE wrapper for a whole cartridge is rejected as -KOS_EINVAL. */
		if (chunk > KOS_IPC_DESC_MAX) {
			chunk = KOS_IPC_DESC_MAX;
		}
		result = read((uint16_t)handle, buffer + done, chunk);
		if (result <= 0) {
			(void)close((uint16_t)handle);
			return result < 0 ? result : -KOS_EIO;
		}
		done = (uint16_t)(done + (uint16_t)result);
	}
	result = close((uint16_t)handle);
	return result < 0 ? result : 0;
}

static uint16_t crc16_ccitt(const uint8_t *data, uint16_t length)
{
	uint16_t crc = 0xFFFFu;
	uint8_t bit;

	while (length != 0u) {
		crc ^= (uint16_t)((uint16_t)*data++ << 8);
		for (bit = 0u; bit != 8u; ++bit) {
			if ((crc & 0x8000u) != 0u) {
				crc = (uint16_t)((crc << 1) ^ 0x1021u);
			} else {
				crc <<= 1;
			}
		}
		--length;
	}
	return crc;
}

static uint8_t main_length(uint8_t op)
{
	if (op == 0x01u || op == 0x11u || op == 0x21u || op == 0x22u ||
	    op == 0x2Au || op == 0x31u || op == 0x32u || op == 0x3Au ||
	    op == 0xC3u || op == 0xCDu) {
		return 3u;
	}
	if (op == 0x06u || op == 0x0Eu || op == 0x10u || op == 0x16u ||
	    op == 0x18u || op == 0x1Eu || op == 0x20u || op == 0x26u ||
	    op == 0x28u || op == 0x2Eu || op == 0x30u || op == 0x36u ||
	    op == 0x38u || op == 0x3Eu || op == 0xD3u || op == 0xDBu) {
		return 2u;
	}
	if (op >= 0xC0u) {
		uint8_t low = (uint8_t)(op & 0x0Fu);
		if (low == 0x02u || low == 0x04u || low == 0x0Au || low == 0x0Cu) {
			return 3u;
		}
		if (low == 0x06u || low == 0x0Eu) {
			return 2u;
		}
	}
	return 1u;
}

static uint8_t index_uses_hl(uint8_t op)
{
	if (op == 0x34u || op == 0x35u || op == 0x36u) {
		return 1u;
	}
	if ((op & 0xC7u) == 0x46u || (op & 0xC7u) == 0x86u) {
		return 1u;
	}
	return (uint8_t)(((op & 0xF8u) == 0x70u && op != 0x76u) ? 1u : 0u);
}

static uint8_t scan_marked(uint16_t offset)
{
	uint8_t mask = (uint8_t)(1u << (offset & 7u));
	return (uint8_t)(work_area.scan.bitmap[offset >> 3] & mask);
}

static void scan_mark(uint16_t offset)
{
	uint8_t mask = (uint8_t)(1u << (offset & 7u));
	work_area.scan.bitmap[offset >> 3] |= mask;
}

static void scan_push_offset(uint16_t offset, uint16_t size,
			     struct scan_report *report)
{
	if (offset >= size || scan_marked(offset)) {
		return;
	}
	if (scan_work_count == SCAN_WORK_ITEMS) {
		report->overflow = 1u;
		return;
	}
	work_area.scan.work[scan_work_count++] = offset;
}

static void scan_push_logical(uint16_t address, uint16_t size,
			      struct scan_report *report)
{
	if (address >= CART_BASE) {
		scan_push_offset((uint16_t)(address - CART_BASE), size, report);
	}
}

static void scan_adapt(uint16_t operand, uint8_t is_out,
		       struct scan_report *report)
{
	uint8_t old = cart[operand];
	uint8_t replacement;

	if (old >= VDP_BLOCK_LO && old <= VDP_BLOCK_HI) {
		replacement = (uint8_t)((old & 1u) != 0u ?
				      ZEPHYR_VDP_COMMAND : ZEPHYR_VDP_DATA);
		if (replacement != old) {
			cart[operand] = replacement;
			++report->vdp;
		}
	} else if (is_out && old >= SOUND_BLOCK_LO) {
		if (old != ZEPHYR_SOUND_PORT) {
			cart[operand] = ZEPHYR_SOUND_PORT;
			++report->sound;
		}
	}
}

static void scan_cartridge(uint16_t size, struct scan_report *report)
{
	uint16_t offset;

	memset(report, 0, sizeof(*report));
	memset(work_area.scan.bitmap, 0, SCAN_BITMAP_BYTES);
	scan_work_count = 0u;
	if (size < 0x24u || !((cart[0] == 0xAAu && cart[1] == 0x55u) ||
			      (cart[0] == 0x55u && cart[1] == 0xAAu))) {
		return;
	}
	report->scanned = 1u;
	scan_push_logical(get_le16(cart + 0x0Au), size, report);
	for (offset = 0x0Cu; offset < 0x24u; offset += 3u) {
		scan_push_offset(offset, size, report);
	}

	while (scan_work_count != 0u) {
		offset = work_area.scan.work[--scan_work_count];
		while (offset < size && !scan_marked(offset)) {
			uint8_t op;
			uint8_t length;
			uint8_t stop = 0u;

			scan_mark(offset);
			op = cart[offset];
			if (op == 0xCBu) {
				length = 2u;
			} else if (op == 0xEDu) {
				uint8_t op2 = (uint8_t)(offset + 1u < size ?
						      cart[offset + 1u] : 0u);
				length = (uint8_t)(((op2 & 0xC7u) == 0x43u) ? 4u : 2u);
				if ((op2 & 0xC7u) == 0x40u ||
				    (op2 & 0xC7u) == 0x41u ||
				    (op2 & 0xE6u) == 0xA2u) {
					++report->indirect;
				}
				stop = (uint8_t)(((op2 & 0xC7u) == 0x45u) ? 1u : 0u);
			} else if (op == 0xDDu || op == 0xFDu) {
				uint8_t op2 = (uint8_t)(offset + 1u < size ?
						      cart[offset + 1u] : 0u);
				if (op2 == 0xCBu) {
					length = 4u;
				} else if (op2 == 0xDDu || op2 == 0xFDu || op2 == 0xEDu) {
					length = 1u;
				} else {
					length = (uint8_t)(1u + main_length(op2) +
							   index_uses_hl(op2));
					stop = (uint8_t)(op2 == 0xE9u ? 1u : 0u);
				}
			} else {
				length = main_length(op);
				if ((op == 0xD3u || op == 0xDBu) && offset + 1u < size) {
					scan_adapt((uint16_t)(offset + 1u),
						   (uint8_t)(op == 0xD3u), report);
				} else if ((op == 0xC3u || op == 0xCDu ||
					    (op & 0xC7u) == 0xC2u ||
					    (op & 0xC7u) == 0xC4u) &&
					   offset + 2u < size) {
					scan_push_logical(get_le16(cart + offset + 1u),
							  size, report);
					if (op == 0xC3u) {
						stop = 1u;
					}
				} else if ((op == 0x18u || op == 0x10u ||
					    (op & 0xE7u) == 0x20u) &&
					   offset + 1u < size) {
					int16_t displacement = (int8_t)cart[offset + 1u];
					uint16_t next = (uint16_t)(offset + length);
					scan_push_offset((uint16_t)(next + displacement),
							 size, report);
					if (op == 0x18u) {
						stop = 1u;
					}
				} else if (op == 0xC9u || op == 0xE9u) {
					stop = 1u;
				}
			}
			if (stop || (uint16_t)(offset + length) > size) {
				break;
			}
			offset = (uint16_t)(offset + length);
		}
	}
}

static int make_pat_path(const char *cart_path, char *path)
{
	char *slash;
	char *dot;

	if (strlcpy(path, cart_path, KOS_PATH_MAX) >= KOS_PATH_MAX) {
		return -KOS_EINVAL;
	}
	slash = strrchr(path, '/');
	dot = strrchr(path, '.');
	if (dot == (char *)0 || (slash != (char *)0 && dot < slash)) {
		if (strlcat(path, ".PAT", KOS_PATH_MAX) >= KOS_PATH_MAX) {
			return -KOS_EINVAL;
		}
	} else {
		if ((uint16_t)(dot - path) > (uint16_t)(KOS_PATH_MAX - 5u)) {
			return -KOS_EINVAL;
		}
		dot[0] = '.';
		dot[1] = 'P';
		dot[2] = 'A';
		dot[3] = 'T';
		dot[4] = '\0';
	}
	return 0;
}

static int apply_manifest(const char *cart_path, uint16_t rounded_length,
			  uint16_t crc, uint16_t *applied)
{
	char path[KOS_PATH_MAX];
	uint32_t size32;
	uint16_t size;
	uint16_t count;
	uint16_t index;
	uint8_t *manifest = work_area.manifest;
	int result;

	*applied = 0u;
	result = make_pat_path(cart_path, path);
	if (result < 0) {
		return result;
	}
	result = file_size(path, &size32);
	if (result == -KOS_ENOENT) {
		return 0;
	}
	if (result < 0) {
		return result;
	}
	/* Keep the 32-bit comparisons separate: SDCC 4.5 otherwise trips an
	 * internal-code-generator bug on the combined short-circuit expression. */
	if (size32 < (uint32_t)PAT_HEADER_BYTES) {
		return -KOS_EINVAL;
	}
	if (size32 > (uint32_t)PAT_MAX_BYTES) {
		return -KOS_EINVAL;
	}
	size = (uint16_t)size32;
	result = load_file(path, manifest, size);
	if (result < 0) {
		return result;
	}
	if (memcmp(manifest, "CGP1", 4u) != 0) {
		return -KOS_EINVAL;
	}
	if (get_le16(manifest + 4u) != rounded_length) {
		return -KOS_EINVAL;
	}
	if (get_le16(manifest + 6u) != crc) {
		return -KOS_EINVAL;
	}
	count = get_le16(manifest + 8u);
	if (count == 0u || count > PAT_MAX_ENTRIES) {
		return -KOS_EINVAL;
	}
	if ((uint16_t)(PAT_HEADER_BYTES + count * PAT_ENTRY_BYTES) > size) {
		return -KOS_EINVAL;
	}
	for (index = 0u; index < count; ++index) {
		uint8_t *entry = manifest + PAT_HEADER_BYTES + index * PAT_ENTRY_BYTES;
		uint16_t offset = get_le16(entry);
		if (offset >= rounded_length) {
			return -KOS_EINVAL;
		}
		if (cart[offset] != entry[2]) {
			return -KOS_EINVAL;
		}
	}
	for (index = 0u; index < count; ++index) {
		uint8_t *entry = manifest + PAT_HEADER_BYTES + index * PAT_ENTRY_BYTES;
		cart[get_le16(entry)] = entry[3];
	}
	*applied = count;
	return 0;
}

static int adapt_bios(void)
{
	uint16_t index;
	uint16_t count = (uint16_t)(sizeof(bios_patches) / sizeof(bios_patches[0]));

	for (index = 0u; index < count; ++index) {
		if (work_area.bios[bios_patches[index].offset] !=
		    bios_patches[index].expected) {
			return -KOS_EINVAL;
		}
	}
	for (index = 0u; index < count; ++index) {
		work_area.bios[bios_patches[index].offset] =
			bios_patches[index].replacement;
	}
	return 0;
}

static void commit_failure(void)
{
	/* Returning to an OS whose target bank has been partly replaced is not
	 * safe.  The kernel's reboot gate does not depend on a filesystem driver. */
	reboot();
}

static void target_write(uint16_t address, const uint8_t *source,
			 uint16_t length)
{
	struct kos_diag_mem_write_args request;

	request.bank = TARGET_BANK;
	while (length != 0u) {
		request.value = *source++;
		request.address = address++;
		if (diag_mem_write(&request) < 0) {
			commit_failure();
		}
		--length;
	}
}

static uint8_t target_read_byte(uint16_t address)
{
	struct kos_diag_mem_read_args request;
	int result;

	request.bank = TARGET_BANK;
	request.reserved = 0u;
	request.address = address;
	result = diag_mem_read(&request);
	if (result < 0) {
		commit_failure();
	}
	return (uint8_t)result;
}

static uint8_t ranges_overlap(uint16_t a, uint16_t a_length,
			      uint16_t b, uint16_t b_length)
{
	return (uint8_t)(a < (uint16_t)(b + b_length) &&
			 b < (uint16_t)(a + a_length));
}

static int validate_handoff_layout(void)
{
	uint16_t handoff_address = (uint16_t)handoff_begin;
	uint16_t handoff_length = (uint16_t)(handoff_end - handoff_begin);
	uint16_t stage_length = (uint16_t)(stage_b_end - stage_b_begin);

	if (handoff_length == 0u || handoff_length > TARGET_STUB_BACKUP_MAX ||
	    handoff_address < KOS_DIAG_MEM_WRITE_MIN ||
	    (uint16_t)(handoff_address + handoff_length) > 0xE000u ||
	    stage_length == 0u || stage_length > TARGET_STAGE_B_BYTES) {
		return -KOS_EINVAL;
	}
	if (ranges_overlap(handoff_address, handoff_length,
			   TARGET_STAGE_B, TARGET_STAGE_B_BYTES) ||
	    ranges_overlap(handoff_address, handoff_length,
			   TARGET_STUB_BACKUP, TARGET_STUB_BACKUP_MAX) ||
	    ranges_overlap(handoff_address, handoff_length,
			   TARGET_BIOS_PAGE, PAGE_BYTES) ||
	    ranges_overlap(handoff_address, handoff_length,
			   TARGET_CART_TAIL, PAGE_BYTES)) {
		return -KOS_EINVAL;
	}
	return 0;
}

static void build_target_and_go(void)
{
	uint16_t handoff_address = (uint16_t)handoff_begin;
	uint16_t handoff_length = (uint16_t)(handoff_end - handoff_begin);
	uint16_t stage_length = (uint16_t)(stage_b_end - stage_b_begin);
	uint16_t index;

	/* Page zero is protected by the diagnostic ABI, so Stage B installs it. */
	target_write(TARGET_BIOS_PAGE, work_area.bios, PAGE_BYTES);
	target_write(PAGE_BYTES, work_area.bios + PAGE_BYTES,
		     (uint16_t)(BIOS_BYTES - PAGE_BYTES));

	/* The upper half is staged low; Stage B copies it through C000h-FFFFh
	 * after selecting KonaOS's flat personality mode. */
	target_write(TARGET_CART_LOWER, cart, CART_HALF_BYTES);
	target_write(TARGET_UPPER_STAGE, cart + CART_HALF_BYTES, CART_HALF_BYTES);

	/* Preserve the final cartridge bytes hidden by the Stage-B body. */
	target_write(TARGET_CART_TAIL,
		     cart + CART_BYTES - PAGE_BYTES, PAGE_BYTES);

	/* Preserve whatever final image belongs under the same-address switch
	 * stub, then overlay the stub.  Stage B restores it after the jump. */
	for (index = 0u; index < handoff_length; ++index) {
		uint8_t value = target_read_byte((uint16_t)(handoff_address + index));
		target_write((uint16_t)(TARGET_STUB_BACKUP + index), &value, 1u);
	}
	target_write(TARGET_STAGE_B, stage_b_begin, stage_length);
	target_write(handoff_address, handoff_begin, handoff_length);

	handoff();
	commit_failure();
}

int main(int argc, char **argv)
{
	const char *cart_path;
	struct scan_report report;
	struct kos_bank_info bank_info;
	struct kos_diag_system_info system_info;
	uint32_t size32;
	uint16_t cart_size;
	uint16_t rounded_size;
	uint16_t cart_crc;
	uint16_t manifest_count;
	int result;

	console_puts("ColecoGo 0.6 for KonaOS\r\n");
	if (argc != 1 || argv[0] == (char *)0 || argv[0][0] == '\0') {
		return fail("usage: colecogo GAME.ROM", 0);
	}
	cart_path = argv[0];

	result = file_size(cart_path, &size32);
	if (result < 0) {
		return fail("cannot stat cartridge", result);
	}
	if (size32 == 0ul || size32 > CART_BYTES) {
		return fail("cartridge must be 1 to 32768 bytes", 0);
	}
	cart_size = (uint16_t)size32;
	rounded_size = (uint16_t)((cart_size + 127u) & 0xFF80u);
	memset(cart, 0xFF, CART_BYTES);
	result = load_file(cart_path, cart, cart_size);
	if (result < 0) {
		return fail("cannot read cartridge", result);
	}
	cart_crc = crc16_ccitt(cart, rounded_size);
	scan_cartridge(rounded_size, &report);
	result = apply_manifest(cart_path, rounded_size, cart_crc, &manifest_count);
	if (result < 0) {
		return fail("invalid or unreadable PAT manifest", result);
	}

	result = file_size("COLECO.ROM", &size32);
	if (result < 0) {
		return fail("cannot stat COLECO.ROM", result);
	}
	if (size32 != BIOS_BYTES) {
		return fail("COLECO.ROM must be exactly 8192 bytes", 0);
	}
	result = load_file("COLECO.ROM", work_area.bios, BIOS_BYTES);
	if (result < 0) {
		return fail("cannot read COLECO.ROM", result);
	}
	if (adapt_bios() < 0) {
		return fail("COLECO.ROM is not the supported standard BIOS", 0);
	}

	result = diag_bank_info(&bank_info);
	if (result < 0) {
		return fail("bank diagnostics are unavailable", result);
	}
	if ((bank_info.mask & (1u << TARGET_BANK)) == 0u) {
		return fail("bank 5 is unavailable", 0);
	}
	if (bank_info.bank == TARGET_BANK) {
		return fail("KonaOS placed ColecoGo in reserved target bank 5", 0);
	}
	result = diag_system_info(&system_info);
	if (result < 0 || system_info.app_mode != 0x10u ||
	    system_info.common_base != 0xE000u || system_info.kernel_bank != 7u) {
		return fail("unsupported KonaOS memory architecture", result);
	}
	if (validate_handoff_layout() < 0) {
		return fail("linked handoff layout is unsafe", 0);
	}

	if (!report.scanned) {
		console_puts("Cartridge has no AA55h header; I/O scan skipped.\r\n");
	} else {
		printf("Cartridge scan: %u VDP and %u sound operand(s) adapted, "
		       "%u indirect site(s) left unpatched.\r\n",
		       report.vdp, report.sound, report.indirect);
		if (report.overflow) {
			console_puts("Warning: trace worklist overflow; coverage is partial.\r\n");
		}
	}
	if (manifest_count != 0u) {
		printf("Patch manifest: %u entry(s) applied.\r\n", manifest_count);
	}
	console_puts("Images validated; starting ColecoVision from bank 5 (no return).\r\n");

	build_target_and_go();
	return 1;
}
