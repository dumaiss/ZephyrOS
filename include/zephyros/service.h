/* ZephyrOS resident driver images (architecture 36.11, work package 41b).
 *
 * A resident driver is a ZMOD container extended with a *service
 * section*: the same 28-byte header, module_type = 2 (service), service
 * ABI 3 in the abi byte.  The section, the segment table, the exported
 * endpoints and the resolved dependencies are described here and pinned
 * by compile-time assertions; `tools/zmodpack.py` links every image at its
 * final bank-6 addresses on the host - the tier plan, the endpoint numbers
 * and the symbolic inter-driver dependencies are all resolved there - and
 * `kernel/service/service_loader.c` only copies each segment to its base,
 * installs the host-built endpoint and dependency tables and runs the
 * entry points.  A container carries no relocation records.
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
 * Each segment is a contiguous image plus a BSS area, linked at its
 * final bank-6 address: the packer applies every 4-byte ZREL_ABS16
 * record on the host (`word += target segment base`, each segment's
 * coordinate space being its image followed by its BSS areas) and the
 * loader copies the bytes verbatim.  Segments may only reach tiers the
 * mode-11 window shows: hot code may not point into boot or bulk code,
 * and bulk segments carry no relocation records.
 *
 * A driver image declares its exported endpoints (class, instance,
 * version, the host-assigned endpoint number, the absolute hot-segment
 * dispatch entry and flags) and its symbolic dependencies on other
 * resident protocol objects (class, instance, optional flag).  The host
 * packer resolves each dependency to the endpoint number of an earlier
 * (or the image's own) export; the loader installs the resulting
 * per-image dependency array before the image's init runs, and a driver
 * never looks a peer up at run time.
 *
 * The service *bootstrap interface* the code calls is `struct
 * ipc_service_api` plus the driver's resolved dependency array, both named
 * by `struct ipc_service_boot` (include/zephyros/ipc.h), a small versioned
 * table of nucleus entry points and one host-resolved reference row, never
 * a kernel import table.
 *
 * The boot manifest (`ZOS_SERVICE_MANIFEST_NAME`) is the flat ROMFS file
 * the bootstrap reader (kernel/romfs) reads: a sequence of 24-byte name
 * records, exactly the ROMFS name field, naming the driver images in
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
#define ZMOD_SERVICE_ABI_VERSION 3u

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

/* Dependency declaration flags: an optional dependency that no image in
 * the build exports resolves to endpoint number 0 (the driver's own
 * fallback); a missing required dependency is a pack-time error. */
#define ZMOD_SERVICE_DEP_OPTIONAL 0x01u

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
 * the pair is the segment's allocation at its final, host-assigned
 * address: `base .. base + image_size + bss_size`, inside the tier of its
 * role.  The payload is `payload_offset .. payload_offset + image_size`
 * in the container payload area, already relocated for `base`.  `entry`
 * is the absolute entry point inside the image (only the boot role may
 * carry one) or ZMOD_NO_ENTRY. */
struct zmod_service_segment {
	uint8_t  role;             /* ZMOD_SEGMENT_*                     */
	uint8_t  flags;            /* 0                                  */
	uint16_t base;             /* final bank-6 address               */
	uint16_t image_size;
	uint16_t bss_size;
	uint16_t entry;            /* absolute boot entry; FFFFh = none  */
	uint16_t payload_offset;   /* relative to the container payload   */
	uint16_t reserved;         /* 0                                  */
};

/* One exported endpoint, 8 bytes.  `number` is the host-assigned endpoint
 * number (1..N across the build's load order) the nucleus table and every
 * resolved dependency reference use; `dispatch` is the absolute hot-tier
 * dispatch entry. */
struct zmod_service_endpoint {
	uint8_t  class;
	uint8_t  instance;
	uint8_t  version;
	uint8_t  flags;            /* ZMOD_SERVICE_EP_*                  */
	uint16_t number;           /* host-assigned endpoint number      */
	uint16_t dispatch;         /* absolute dispatch entry            */
};

/* One resolved dependency, 4 bytes.  The host resolved the declared
 * (class, instance) to `number`, the endpoint number of an earlier (or
 * this image's own) export; 0 means the declaration was optional and no
 * image in the build exports it. */
struct zmod_service_dependency {
	uint8_t  class;
	uint8_t  instance;
	uint16_t number;           /* resolved endpoint number; 0 = absent */
};

/* Pinned on-disk sizes; the assertions below keep the structures and
 * these numbers in step, and the packer uses the same values. */
#define ZMOD_SERVICE_HEADER_SIZE     16u
#define ZMOD_SERVICE_SEGMENT_SIZE    14u
#define ZMOD_SERVICE_ENDPOINT_SIZE   8u
#define ZMOD_SERVICE_DEPENDENCY_SIZE 4u

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
 *   init_offset        absolute hot-segment init entry; ZMOD_NO_ENTRY = none
 *   fini_offset        ZMOD_NO_ENTRY (services are permanent; no unload)
 *   reloc_offset       0 (host-linked: no relocation records)
 *   reloc_count        0
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
				  base) == 0x02,
	       "service segment base offset");
_Static_assert(__builtin_offsetof(struct zmod_service_segment,
				  image_size) == 0x04,
	       "service segment image_size offset");
_Static_assert(__builtin_offsetof(struct zmod_service_segment,
				  entry) == 0x08,
	       "service segment entry offset");
_Static_assert(__builtin_offsetof(struct zmod_service_segment,
				  payload_offset) == 0x0A,
	       "service segment payload_offset offset");
_Static_assert(sizeof(struct zmod_service_endpoint) ==
	       ZMOD_SERVICE_ENDPOINT_SIZE,
	       "service endpoint must be 8 bytes");
_Static_assert(__builtin_offsetof(struct zmod_service_endpoint,
				  number) == 0x04,
	       "service endpoint number offset");
_Static_assert(__builtin_offsetof(struct zmod_service_endpoint,
				  dispatch) == 0x06,
	       "service endpoint dispatch offset");
_Static_assert(sizeof(struct zmod_service_dependency) ==
	       ZMOD_SERVICE_DEPENDENCY_SIZE,
	       "service dependency must be 4 bytes");
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
