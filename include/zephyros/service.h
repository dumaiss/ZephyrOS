/* ZephyrOS resident driver images (architecture 36.11, work package 42c).
 *
 * A resident driver is a ZMOD service image: a flat, host-linked load
 * plan the nucleus loader iterates with protective bounds only.  The host
 * packer (tools/zmodpack.py) links every segment at its final bank-6
 * address, assigns the endpoint numbers and resolves the symbolic
 * dependencies, and emits:
 *
 *   header            magic, the table counts, the init entry, the
 *                     payload offset and the exact container size
 *   segment table     role, base, image/BSS sizes, payload offset and the
 *                     optional boot entry (one record per segment)
 *   endpoint table    class/instance/version/flags, the host-assigned
 *                     number and the absolute hot dispatch entry
 *   dependency table  the resolved endpoint numbers
 *   publication table the `/dev` and `/FS` namespace names
 *   payload           the linked segment images, concatenated
 *
 * There is no version field, no module type, no import table, no fini
 * entry and no relocation record: the packer validates all of it on the
 * host (M10-REFERENCE decision 2), so the loader checks only the bounds
 * that protect the nucleus and the fixed driver-bank layout.
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
 * record on the host and the loader copies the bytes verbatim.  Segments
 * may only reach tiers the mode-11 window shows: hot code may not point
 * into boot or bulk code, and bulk segments carry no relocation records.
 *
 * The service *bootstrap interface* the code calls is `struct
 * ipc_service_api` plus the driver's resolved dependency array, both
 * named by `struct ipc_service_boot` (include/zephyros/ipc.h), a small
 * table of nucleus entry points and one host-resolved reference row,
 * never a kernel import table.
 *
 * The boot manifest (`ZOS_SERVICE_MANIFEST_NAME`) is the flat ROMFS file
 * the bootstrap reader (kernel/romfs) reads: a sequence of 24-byte name
 * records, exactly the ROMFS name field, naming the driver images in
 * load order.  Flat constant names only: no path syntax, no directory.
 */

#ifndef ZEPHYROS_SERVICE_H
#define ZEPHYROS_SERVICE_H

#include "zephyros/ipc.h"
#include "zephyros/romfs.h"
#include "zos/types.h"

/* --- the flat container ------------------------------------------------ */

#define ZMOD_MAGIC_0 0x5Au /* 'Z' */
#define ZMOD_MAGIC_1 0x4Du /* 'M' */
#define ZMOD_MAGIC_2 0x4Fu /* 'O' */
#define ZMOD_MAGIC_3 0x44u /* 'D' */

#define ZMOD_HEADER_SIZE      16u
#define ZMOD_SEGMENT_SIZE     12u
#define ZMOD_ENDPOINT_SIZE    8u
#define ZMOD_DEPENDENCY_SIZE  4u
#define ZMOD_PUBLICATION_SIZE 20u

#define ZMOD_NO_ENTRY 0xFFFFu

/* Format maxima (the loader's tables and the packer agree). */
#define ZMOD_MAX_SEGMENTS     4u
#define ZMOD_MAX_ENDPOINTS    8u
#define ZMOD_MAX_DEPENDENCIES 8u
/* Publications per image; the nucleus publication table is smaller (the
 * loader refuses a set that overflows it, VFS_NS_PUB_MAX). */
#define ZMOD_MAX_PUBLICATIONS 12u

/* Boot manifest bound: the loader's per-image loop. */
#define ZMOD_MAX_IMAGES       8u

/* Segment roles.  At most one segment per role; `hot` is required. */
#define ZMOD_SEGMENT_HOT  1u
#define ZMOD_SEGMENT_BOOT 2u
#define ZMOD_SEGMENT_IO   3u
#define ZMOD_SEGMENT_BULK 4u

/* Endpoint flags: bit 0 is the re-entrancy declaration, the same bit the
 * endpoint table keeps (ZOS_IPC_EP_REENTRANT, abi/zos/ipc.h). */
#define ZMOD_EP_REENTRANT ZOS_IPC_EP_REENTRANT

/* Container header, 16 bytes, little-endian.  `payload_offset` is the
 * first payload byte and equals the end of the tables; `total_size` is
 * the exact container length (the tables and the payload end there). */
struct zmod_header {
	uint8_t  magic[4];
	uint8_t  segment_count;
	uint8_t  endpoint_count;
	uint8_t  dependency_count;
	uint8_t  publication_count;
	uint16_t init_entry;      /* absolute hot-segment init; FFFFh = none */
	uint16_t payload_offset;
	uint16_t total_size;
	uint16_t reserved;        /* 0 */
};

/* One segment, 12 bytes.  `base` is the host-assigned final bank-6
 * address; the image is `payload_offset .. payload_offset + image_size`
 * in the container payload, and the BSS area follows the image at
 * `base + image_size`.  `entry` is the absolute boot entry (only the
 * boot role may carry one) or ZMOD_NO_ENTRY. */
struct zmod_segment {
	uint8_t  role;            /* ZMOD_SEGMENT_*                    */
	uint8_t  reserved;        /* 0                                 */
	uint16_t base;
	uint16_t image_size;
	uint16_t bss_size;
	uint16_t entry;
	uint16_t payload_offset;  /* relative to the container payload */
};

/* One exported endpoint, 8 bytes.  `number` is the host-assigned endpoint
 * number (1..N across the build's load order); `dispatch` is the
 * absolute hot-tier dispatch entry. */
struct zmod_endpoint {
	uint8_t  class;
	uint8_t  instance;
	uint8_t  version;
	uint8_t  flags;           /* ZMOD_EP_*                         */
	uint16_t number;
	uint16_t dispatch;
};

/* One resolved dependency, 4 bytes.  The host resolved the declared
 * (class, instance) to `number`, the endpoint number of an earlier (or
 * this image's own) export; 0 means the declaration was optional and no
 * image in the build exports it. */
struct zmod_dependency {
	uint8_t  class;
	uint8_t  instance;
	uint16_t number;
};

/* One publication, 20 bytes.  `name` is the provider-relative namespace
 * name below the DEVICE provider's `/dev` root (`"/console"`),
 * NUL-terminated and zero-padded; `type` is the 8-bit protocol type
 * (abi/zos/object.h ZOS_TYPE_*); `number` is the serving endpoint's
 * host-assigned number; `object` is the provider's own object identity,
 * which a nucleus provider receives with every call to the publication.
 * A host build never publishes a grouping name: the kernel derives the
 * container rows (`/input`, `/serial`) from the names themselves. */
struct zmod_publication {
	char     name[16];        /* "/console", NUL-terminated */
	uint8_t  type;            /* ZOS_TYPE_*                */
	uint8_t  number;          /* serving endpoint number   */
	uint8_t  object;          /* provider object identity  */
	uint8_t  flags;           /* 0                         */
};

#if defined(__SDCC)
_Static_assert(sizeof(struct zmod_header) == ZMOD_HEADER_SIZE,
	       "zmod header must be 16 bytes");
_Static_assert(__builtin_offsetof(struct zmod_header, segment_count) == 4,
	       "zmod header segment_count offset");
_Static_assert(__builtin_offsetof(struct zmod_header, init_entry) == 8,
	       "zmod header init_entry offset");
_Static_assert(__builtin_offsetof(struct zmod_header, payload_offset) == 10,
	       "zmod header payload_offset offset");
_Static_assert(sizeof(struct zmod_segment) == ZMOD_SEGMENT_SIZE,
	       "zmod segment must be 12 bytes");
_Static_assert(__builtin_offsetof(struct zmod_segment, base) == 2,
	       "zmod segment base offset");
_Static_assert(__builtin_offsetof(struct zmod_segment, entry) == 8,
	       "zmod segment entry offset");
_Static_assert(sizeof(struct zmod_endpoint) == ZMOD_ENDPOINT_SIZE,
	       "zmod endpoint must be 8 bytes");
_Static_assert(__builtin_offsetof(struct zmod_endpoint, number) == 4,
	       "zmod endpoint number offset");
_Static_assert(__builtin_offsetof(struct zmod_endpoint, dispatch) == 6,
	       "zmod endpoint dispatch offset");
_Static_assert(sizeof(struct zmod_dependency) == ZMOD_DEPENDENCY_SIZE,
	       "zmod dependency must be 4 bytes");
_Static_assert(sizeof(struct zmod_publication) == ZMOD_PUBLICATION_SIZE,
	       "zmod publication must be 20 bytes");
#endif /* __SDCC */

/* Loader error codes (0 = success, negative = failure). */
#define ZMOD_OK       0
#define ZMOD_EINVAL   (-ZOS_EINVAL)  /* invalid argument/field value       */
#define ZMOD_EFORMAT  (-ZOS_EIO)     /* corrupt/truncated/overlapping data */
#define ZMOD_ENOTSUP  (-ZOS_ENOTSUP) /* unsupported version/type/reloc     */
#define ZMOD_ENOSPC   (-ZOS_ENOSPC)  /* table or publication exhaustion    */
#define ZMOD_ENOENT   (-ZOS_ENOENT)  /* no such manifest/file              */
#define ZMOD_EBUSY    (-ZOS_EBUSY)   /* nucleus table refused the record   */

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

#endif /* ZEPHYROS_SERVICE_H */
