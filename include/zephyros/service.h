/* ZephyrOS resident service images (architecture 36.11, work package 40).
 *
 * A resident service is a ZMOD container extended with a *service
 * section*: the same 28-byte header, module_type = 2 (service), service
 * ABI in the abi byte.  The section, the segment table, the exported
 * endpoints and the declared dependencies are described here and pinned
 * by compile-time assertions; `tools/zmodpack.py` validates and packs the
 * same format on the host and pre-computes the layout, and
 * `kernel/service/service_loader.c` places the image with the existing
 * ZMOD relocation records (ZREL_ABS16) into the bank-6 tiers.
 *
 * Segments and destination tiers (architecture 36.6):
 *
 *   hot    6000h-DFFFh of bank 6   run-time code and data, reached
 *                                  through the mode-11 window (P = 1)
 *   boot   0000h-5FFFh of bank 6   boot-only code, run once through the
 *                                  mode-10 gate with Q = 6
 *   io     8000h-BFFFh of bank 6   SERVICE_IO buffers: the sources and
 *                                  targets of copy_in/copy_out (36.8)
 *   bulk   0000h-5FFFh of bank 6   bulk data, reached only by nucleus
 *                                  copies
 *
 * Each segment is a contiguous image plus a BSS area; its relocation
 * records are the existing 4-byte ZREL_ABS16 records, indexed by
 * `patch_offset` relative to the segment's image.  In a service
 * container the record's second byte names the *target segment* and the
 * loader applies `word += target segment base`, exactly the driver
 * arena's rule with each segment relocated independently, so hot code
 * can patch in the addresses of its io buffers.  The driver arena keeps
 * the byte reserved (0) because it is one contiguous arena.  Segments
 * may only reach tiers the mode-11 window shows: hot code may not point
 * into boot or bulk code, and bulk segments carry no relocation records.
 *
 * A service image declares its exported endpoints (class, instance,
 * version, hot-image dispatch offset, flags) and the endpoints it
 * depends on (class, instance).  The resident loader registers the
 * declared endpoints before any service code runs and refuses an image
 * whose declared dependencies are not registered yet, in manifest order.
 *
 * The service *bootstrap interface* the code calls is `struct
 * ipc_service_api` (include/zephyros/ipc.h), a small versioned table of
 * nucleus entry points, never a kernel import table.
 *
 * The boot manifest (`ZOS_SERVICE_MANIFEST_NAME`) is the flat ROMFS file
 * the bootstrap reader (kernel/romfs) reads: a sequence of 24-byte name
 * records, exactly the ROMFS name field, naming the service images in
 * load order.  Flat constant names only: no path syntax, no directory.
 */

#ifndef ZEPHYROS_SERVICE_H
#define ZEPHYROS_SERVICE_H

#include "zephyros/ipc.h"
#include "zephyros/module.h"
#include "zephyros/romfs.h"
#include "zos/types.h"

/* ZMOD module type for a resident service container. */
#define ZMOD_TYPE_SERVICE 2u

/* Service ABI carried in the header's abi byte (header offset 06h). */
#define ZMOD_SERVICE_ABI_VERSION 1u

/* The service section always starts directly after the container header. */
#define ZMOD_SERVICE_SECTION_OFFSET ZMOD_HEADER_SIZE

/* Segment roles.  At most one segment per role; `hot` is required. */
#define ZMOD_SEGMENT_HOT  1u
#define ZMOD_SEGMENT_BOOT 2u
#define ZMOD_SEGMENT_IO   3u
#define ZMOD_SEGMENT_BULK 4u

/* Endpoint flags: bit 0 is the re-entrancy declaration, the same bit the
 * endpoint table keeps (ZOS_IPC_EP_REENTRANT, abi/zos/ipc.h). */
#define ZMOD_SERVICE_EP_REENTRANT ZOS_IPC_EP_REENTRANT

/* Service section header, 16 bytes, little-endian.  All table offsets are
 * relative to the section start (ZMOD_SERVICE_SECTION_OFFSET) and every
 * table lies inside [section start, section start + section_size). */
struct zmod_service_header {
	uint16_t section_size;      /* bytes of this section, tables included */
	uint16_t segment_offset;    /* segment table, relative to the section */
	uint16_t endpoint_offset;   /* endpoint table, relative to the section */
	uint16_t dependency_offset; /* dependency table, relative to the section */
	uint8_t  segment_count;     /* 0 .. ZMOD_SERVICE_MAX_SEGMENTS */
	uint8_t  endpoint_count;    /* 0 .. ZMOD_SERVICE_MAX_ENDPOINTS */
	uint8_t  dependency_count;  /* 0 .. ZMOD_SERVICE_MAX_DEPENDENCIES */
	uint8_t  reserved;          /* 0 */
	uint16_t reserved2;         /* 0 */
	uint16_t reserved3;         /* 0 */
};

/* One segment, 14 bytes.  `image_size` + `bss_size` must be non-zero and
 * the pair is the segment's allocation; the payload is
 * `payload_offset .. payload_offset + image_size` in the container payload
 * area, and the segment's relocation records are the `reloc_count`
 * records starting at index `reloc_start` in the container's one ZREL
 * table.  `entry_offset` names an entry point inside the image (only the
 * boot role may carry one) or ZMOD_NO_ENTRY. */
struct zmod_service_segment {
	uint8_t  role;             /* ZMOD_SEGMENT_*                     */
	uint8_t  flags;            /* 0                                  */
	uint16_t image_size;
	uint16_t bss_size;
	uint16_t entry_offset;     /* boot entry; ZMOD_NO_ENTRY = none    */
	uint16_t payload_offset;   /* relative to the container payload   */
	uint16_t reloc_start;      /* first relocation record            */
	uint16_t reloc_count;
};

/* One exported endpoint, 8 bytes.  `dispatch_offset` is relative to the
 * hot segment's image; the resident loader registers
 * hot_base + dispatch_offset. */
struct zmod_service_endpoint {
	uint8_t  class;
	uint8_t  instance;
	uint8_t  version;
	uint8_t  flags;            /* ZMOD_SERVICE_EP_*                  */
	uint16_t dispatch_offset;
	uint16_t reserved;         /* 0                                  */
};

/* One declared dependency, 2 bytes.  The loader requires an endpoint
 * with the same (class, instance) to be registered before the image is
 * loaded. */
struct zmod_service_dependency {
	uint8_t  class;
	uint8_t  instance;
};

/* Pinned on-disk sizes; the assertions below keep the structures and
 * these numbers in step, and the packer uses the same values. */
#define ZMOD_SERVICE_HEADER_SIZE     16u
#define ZMOD_SERVICE_SEGMENT_SIZE    14u
#define ZMOD_SERVICE_ENDPOINT_SIZE   8u
#define ZMOD_SERVICE_DEPENDENCY_SIZE 2u

/* Format maxima (the loader's tables and the packer agree). */
#define ZMOD_SERVICE_MAX_SEGMENTS     4u
#define ZMOD_SERVICE_MAX_ENDPOINTS    8u
#define ZMOD_SERVICE_MAX_DEPENDENCIES 8u

/* Boot manifest bound: the loader's per-image table. */
#define ZMOD_SERVICE_MAX_IMAGES       8u

/* Header field semantics for module_type = SERVICE (ZMOD header offsets
 * are unchanged):
 *   image_size         total payload bytes (all segment images)
 *   bss_size           total BSS bytes (all segment BSS areas)
 *   required_granules  0: per-tier allocation is the loader's, computed
 *                      from the segment table, so the driver granule field
 *                      is unused and must be zero
 *   init_offset        hot-segment-relative init entry; ZMOD_NO_ENTRY = none
 *   fini_offset        ZMOD_NO_ENTRY (services are permanent; no unload)
 *   reloc_offset       the single ZREL_ABS16 record table
 *   reloc_count        records in that table
 *   payload_offset     payload area (segment images concatenated)
 *   total_size         exact container length
 *   reserved           0
 */

#if defined(__SDCC)
_Static_assert(sizeof(struct zmod_service_header) ==
	       ZMOD_SERVICE_HEADER_SIZE,
	       "service section header must be 16 bytes");
_Static_assert(__builtin_offsetof(struct zmod_service_header,
				  section_size) == 0x00,
	       "service section header section_size offset");
_Static_assert(__builtin_offsetof(struct zmod_service_header,
				  segment_offset) == 0x02,
	       "service section header segment_offset offset");
_Static_assert(__builtin_offsetof(struct zmod_service_header,
				  endpoint_offset) == 0x04,
	       "service section header endpoint_offset offset");
_Static_assert(__builtin_offsetof(struct zmod_service_header,
				  dependency_offset) == 0x06,
	       "service section header dependency_offset offset");
_Static_assert(__builtin_offsetof(struct zmod_service_header,
				  segment_count) == 0x08,
	       "service section header segment_count offset");
_Static_assert(__builtin_offsetof(struct zmod_service_header,
				  dependency_count) == 0x0A,
	       "service section header dependency_count offset");
_Static_assert(sizeof(struct zmod_service_segment) ==
	       ZMOD_SERVICE_SEGMENT_SIZE,
	       "service segment must be 14 bytes");
_Static_assert(__builtin_offsetof(struct zmod_service_segment,
				  image_size) == 0x02,
	       "service segment image_size offset");
_Static_assert(__builtin_offsetof(struct zmod_service_segment,
				  entry_offset) == 0x06,
	       "service segment entry_offset offset");
_Static_assert(__builtin_offsetof(struct zmod_service_segment,
				  payload_offset) == 0x08,
	       "service segment payload_offset offset");
_Static_assert(__builtin_offsetof(struct zmod_service_segment,
				  reloc_start) == 0x0A,
	       "service segment reloc_start offset");
_Static_assert(sizeof(struct zmod_service_endpoint) ==
	       ZMOD_SERVICE_ENDPOINT_SIZE,
	       "service endpoint must be 8 bytes");
_Static_assert(__builtin_offsetof(struct zmod_service_endpoint,
				  dispatch_offset) == 0x04,
	       "service endpoint dispatch_offset offset");
_Static_assert(sizeof(struct zmod_service_dependency) ==
	       ZMOD_SERVICE_DEPENDENCY_SIZE,
	       "service dependency must be 2 bytes");
#endif /* __SDCC */

/* The flat ROMFS name of the boot manifest.  The bootstrap reader
 * (kernel/romfs) resolves it like any other flat name, without path
 * syntax; every read is staged through common memory. */
#define ZOS_SERVICE_MANIFEST_NAME "services.man"

/* Manifest record: exactly the ROMFS name field, NUL-terminated and
 * zero-padded.  The manifest file's size is a multiple of this. */
#define ZOS_SERVICE_MANIFEST_ENTRY ROMFS_NAME_SIZE

/* --- the resident loader (kernel/service/service_loader.c) --------------
 *
 * Cold nucleus code: read the boot manifest and bring up every service
 * image it names, in load order.  Returns 0 or a negative ZMOD/ZOS error;
 * a failure is a boot error and is reported on the early console.
 * Services are permanent (M10-REFERENCE decision 11), so a second call is
 * a no-op. */
int service_load_all(const char *manifest_name);

/* True once the manifest has been loaded. */
uint8_t service_images_loaded(void);

/* The architecture-36.20 ledgers: allocated bytes in each bank-6 tier
 * (hot includes the SERVICE_IO blocks, which live in the hot tier). */
uint16_t service_hot_used(void);
uint16_t service_boot_used(void);

#endif /* ZEPHYROS_SERVICE_H */
