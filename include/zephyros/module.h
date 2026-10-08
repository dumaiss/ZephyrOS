/* ZephyrOS module loader contract (ZMOD v1 containers, driver ABI v3).
 *
 * This header defines:
 *   - the ZMOD v1 on-disk container (packed little-endian, 28-byte header),
 *   - loader error codes and module state,
 *   - the embedded module catalog interface,
 *   - the kernel loader/allocator API used by the M4 self-test and,
 *     from stage 08 on, by catalog loading at boot.
 *
 * ZMOD is a generic relocatable container; module types DRIVER (driver
 * ABI v3) and, from M10 on, SERVICE (service ABI v1, see
 * include/zephyros/service.h) are supported (older ABI containers are
 * rejected).  All addresses stored inside a loaded image are offsets
 * relative to the module allocation base; the loader patches ZREL_ABS16
 * records with the chosen load address.
 *
 * The on-disk structures are byte-packed little-endian.  SDCC z80 does
 * not insert padding, so the structs may be overlaid on container bytes;
 * the compile-time assertions below pin the layout.
 */

#ifndef ZEPHYROS_MODULE_H
#define ZEPHYROS_MODULE_H

#include "zos/types.h"
#include "zephyros/errno.h"

/* --- ZMOD v1 container ------------------------------------------------ */

#define ZMOD_MAGIC_0 0x5Au /* 'Z' */
#define ZMOD_MAGIC_1 0x4Du /* 'M' */
#define ZMOD_MAGIC_2 0x4Fu /* 'O' */
#define ZMOD_MAGIC_3 0x44u /* 'D' */

#define ZMOD_FORMAT_VERSION 1u
#define ZMOD_TYPE_DRIVER    1u
#define ZMOD_FLAG_NONE      0u

#define ZMOD_HEADER_SIZE 28u
#define ZMOD_RELOC_SIZE  4u

/* Relocation types.  A record is { type, target, patch offset word }: the
 * second byte is reserved (0) in driver containers, where one base covers
 * the whole image, and names the target segment in service containers
 * (include/zephyros/service.h), which relocate each segment separately. */
#define ZREL_ABS16 1u

#define ZMOD_NO_ENTRY 0xFFFFu

/* On-disk header, little-endian, packed.  Field offsets are part of the
 * format; see docs/module-format.md.
 *
 *   00 magic[4]="ZMOD"   04 format_version     05 module_type
 *   06 driver_abi        07 flags
 *   08 image_size        0A bss_size           0C required_granules
 *   0E init_offset       10 fini_offset (FFFFh=none)
 *   12 reloc_offset      14 reloc_count
 *   16 payload_offset    18 total_size
 *   1A reserved (=0)
 */
struct zmod_header {
	uint8_t  magic[4];
	uint8_t  format_version;
	uint8_t  module_type;
	uint8_t  driver_abi;
	uint8_t  flags;
	uint16_t image_size;
	uint16_t bss_size;
	uint16_t required_granules;
	uint16_t init_offset;
	uint16_t fini_offset;
	uint16_t reloc_offset;
	uint16_t reloc_count;
	uint16_t payload_offset;
	uint16_t total_size;
	uint16_t reserved;
};

/* One relocation record.  patch_offset is relative to the loaded image. */
struct zmod_reloc {
	uint8_t  type;
	uint8_t  reserved;
	uint16_t patch_offset;
};

#if defined(__SDCC)
_Static_assert(sizeof(struct zmod_header) == ZMOD_HEADER_SIZE,
	       "ZMOD header must be 28 bytes");
_Static_assert(__builtin_offsetof(struct zmod_header, magic) == 0x00,
	       "ZMOD magic offset");
_Static_assert(__builtin_offsetof(struct zmod_header, format_version) == 0x04,
	       "ZMOD format_version offset");
_Static_assert(__builtin_offsetof(struct zmod_header, module_type) == 0x05,
	       "ZMOD module_type offset");
_Static_assert(__builtin_offsetof(struct zmod_header, driver_abi) == 0x06,
	       "ZMOD driver_abi offset");
_Static_assert(__builtin_offsetof(struct zmod_header, flags) == 0x07,
	       "ZMOD flags offset");
_Static_assert(__builtin_offsetof(struct zmod_header, image_size) == 0x08,
	       "ZMOD image_size offset");
_Static_assert(__builtin_offsetof(struct zmod_header, bss_size) == 0x0A,
	       "ZMOD bss_size offset");
_Static_assert(__builtin_offsetof(struct zmod_header, required_granules) == 0x0C,
	       "ZMOD required_granules offset");
_Static_assert(__builtin_offsetof(struct zmod_header, init_offset) == 0x0E,
	       "ZMOD init_offset offset");
_Static_assert(__builtin_offsetof(struct zmod_header, fini_offset) == 0x10,
	       "ZMOD fini_offset offset");
_Static_assert(__builtin_offsetof(struct zmod_header, reloc_offset) == 0x12,
	       "ZMOD reloc_offset offset");
_Static_assert(__builtin_offsetof(struct zmod_header, reloc_count) == 0x14,
	       "ZMOD reloc_count offset");
_Static_assert(__builtin_offsetof(struct zmod_header, payload_offset) == 0x16,
	       "ZMOD payload_offset offset");
_Static_assert(__builtin_offsetof(struct zmod_header, total_size) == 0x18,
	       "ZMOD total_size offset");
_Static_assert(__builtin_offsetof(struct zmod_header, reserved) == 0x1A,
	       "ZMOD reserved offset");
_Static_assert(sizeof(struct zmod_reloc) == ZMOD_RELOC_SIZE,
	       "ZMOD reloc must be 4 bytes");
_Static_assert(__builtin_offsetof(struct zmod_reloc, patch_offset) == 0x02,
	       "ZMOD reloc patch_offset offset");
#endif /* __SDCC */

/* --- loader policy ---------------------------------------------------- */

/* Granule geometry.  Allocation granularity is fixed at 512 bytes; the
 * runtime granule count is derived from the linked driver-arena symbols
 * (`__driver_arena_start`/`__driver_arena_end`) and bounds every loader
 * table.  `ZMOD_MAX_GRANULES` stays the format's conservative upper bound
 * (`(ZMOD_ARCH_END-ZMOD_ARCH_BASE)/512`) even though the package-38 layout
 * caps the arena at the cold half's 64 granules: the module format is
 * unchanged, and the real bound is the linked arena (`tools/mapcheck.py`
 * derives it and `zmodpack plan` must fit the boot set inside it).  There
 * is deliberately no compiled arena count: the arena is derived from the
 * aligned end of the linked cold code and the derived cold runtime-data
 * base (both are validated against these bounds). */
#define ZMOD_GRANULE_SIZE 512u
#define ZMOD_ARCH_BASE     0x2000u
#define ZMOD_ARCH_END      0xDF00u
#define ZMOD_MAX_GRANULES  95u
#define ZMOD_MAX_MODULES   16u
#define ZMOD_MAX_TOTAL_SIZE 0xFFFFu

/* Loader/validation error codes (0 = success, negative = failure). */
#define ZMOD_OK       0
#define ZMOD_EINVAL   (-ZOS_EINVAL)  /* invalid argument/field value       */
#define ZMOD_EFORMAT  (-ZOS_EIO)     /* corrupt/truncated/overlapping data */
#define ZMOD_ENOTSUP  (-ZOS_ENOTSUP) /* unsupported version/type/reloc     */
#define ZMOD_ENOSPC   (-ZOS_ENOSPC)  /* arena or descriptor exhaustion     */
#define ZMOD_ENOENT   (-ZOS_ENOENT)  /* no such catalog entry              */
#define ZMOD_EINIT    (-ZOS_EINIT)   /* module init refused to start       */

/* Module lifecycle.  A failed load returns to EMPTY after its allocation
 * has been rolled back; no public unload exists in M4. */
enum {
	ZMOD_STATE_EMPTY = 0,
	ZMOD_STATE_LOADING,
	ZMOD_STATE_RESIDENT
};

/* Bounded container byte source (M5): the loader reads every container
 * byte through this adapter, so a container may live in memory or in the
 * ROMFS recovery store.  `read` returns the number of bytes copied
 * (equal to `length`) or a negative error; it must never read outside
 * [0, size). */
typedef int (*zmod_source_read_fn)(const void *ctx, uint16_t offset,
				   uint8_t *dst, uint16_t length);

struct zmod_source {
	zmod_source_read_fn read;
	const void *ctx;
	uint16_t size; /* total container bytes */
};

/* Validated container summary (offsets are relative to container start).
 * Exactly one byte source is recorded: `blob` for memory containers or
 * `source` for bounded read-source containers (M5 ROMFS). */
struct zmod_container {
	const uint8_t *blob;
	const struct zmod_source *source;
	uint16_t total_size;
	uint16_t image_size;
	uint16_t bss_size;
	uint16_t required_granules;
	uint16_t init_offset;
	uint16_t fini_offset; /* ZMOD_NO_ENTRY when absent */
	uint16_t reloc_offset;
	uint16_t reloc_count;
	uint16_t payload_offset;
};

/* Loader bookkeeping for one loaded (or loading) module.  Stored in
 * bank-7 kernel data; catalog names remain owned by the catalog. */
struct zmod_module {
	uint8_t state;
	uint8_t first_granule;
	uint8_t granule_count;
	const char *name;
	const uint8_t *container;
	uint16_t load_base;
	uint16_t image_size;
	uint16_t bss_size;
	uint16_t init_offset;
	uint16_t fini_offset;
};

/* --- kernel loader API ------------------------------------------------ */

/* Validate arena geometry and reset allocator/descriptor state.  Must be
 * called once before loading; returns ZMOD_OK or a negative error. */
int module_loader_init(void);

/* Bounded read-source load: validate the container, allocate granules,
 * stream/copy and relocate the image, zero BSS, and run the module init
 * with driver ABI v3.  On any failure the allocation, device
 * registrations, and IRQ claims made by the module are rolled back and
 * the descriptor is freed.  The entire container structure is validated
 * before the arena is touched; the container bytes are read only through
 * `source`.  This is the only load path an image uses.  Returns the
 * module index (>= 0) or a negative error. */
int module_load_source(const char *name, const struct zmod_source *source);

uint8_t module_count(void);
uint8_t module_free_granules(void);
uint8_t module_granule_count(void);
uint8_t module_failed_count(void);
const struct zmod_module *module_get(uint8_t index);

/* IRQ handler-address validation hook: true when `address` lies inside a
 * RESIDENT module image, or inside the currently initializing (LOADING)
 * module image.  Failed/freed and out-of-arena addresses return false. */
uint8_t module_code_allowed(uint16_t address);

/* Internal relocation/validation interface (kernel/module/relocate.c),
 * shared with the target self-test through this header. */
int zmod_validate_source(const struct zmod_source *source,
			 struct zmod_container *out);
int zmod_copy_image(uint8_t *load_base, const struct zmod_container *c);
void zmod_zero_bss(uint8_t *load_base, const struct zmod_container *c);
int zmod_apply_relocations(uint8_t *load_base,
			   const struct zmod_container *c);
void zmod_zero_allocation(uint8_t *load_base, uint16_t bytes);

#endif /* ZEPHYROS_MODULE_H */
