/* KonaOS resident driver images (architecture 5.3).
 *
 * A resident driver is a MOD driver image: a flat, host-linked load
 * plan the nucleus loader iterates with protective bounds only.  The host
 * packer (tools/modpack.py) links every segment at its final bank-6
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
 *   publication table the `/dev` and `/fs` namespace names
 *   payload           the linked segment images, concatenated
 *
 * There is no version field, no module type, no import table, no fini
 * entry and no relocation record: the packer validates all of it on the
 * host, so the loader checks only the bounds
 * that protect the nucleus and the fixed driver-bank layout.
 *
 * Segments and destination tiers (architecture 5.2):
 *
 *   hot    6000h-DFFFh of bank 6   run-time code and data, reached
 *                                  through the mode-11 window (P = 1)
 *   boot   0000h-5FFFh of bank 6   boot-only code, run once through the
 *                                  mode-10 gate with Q = 6
 *   io     8000h-BFFFh of bank 6   DRIVER_IO buffers: the sources and
 *                                  targets of copy_in/copy_out (4.9)
 *   bulk   0000h-5FFFh of bank 6   bulk data, reached only by nucleus
 *                                  copies
 *
 * Each segment is a contiguous image plus a BSS area, linked at its
 * final bank-6 address: the packer applies every 4-byte KREL_ABS16
 * record on the host and the loader copies the bytes verbatim.  Segments
 * may only reach tiers the mode-11 window shows: hot code may not point
 * into boot or bulk code, and bulk segments carry no relocation records.
 *
 * The driver *bootstrap interface* the code calls is `struct
 * driver_api` plus the driver's resolved dependency array, both
 * named by `struct driver_boot` (drivers/include/driver.h), a small
 * table of nucleus entry points and one host-resolved reference row,
 * never a kernel import table.
 *
 * The boot manifest (`KOS_DRIVER_MANIFEST_NAME`) is the flat ROMFS file
 * the bootstrap reader (kernel/romfs) reads: a sequence of 24-byte name
 * records, exactly the ROMFS name field, naming the driver images in
 * load order.  Flat constant names only: no path syntax, no directory.
 */

#ifndef KONAOS_DRIVER_H
#define KONAOS_DRIVER_H

#include "driver.h"
#include "romfs.h"
#include "kos/types.h"

/* --- the flat container ------------------------------------------------ */

#define MOD_MAGIC_0 0x4Bu /* 'K' */
#define MOD_MAGIC_1 0x4Du /* 'M' */
#define MOD_MAGIC_2 0x4Fu /* 'O' */
#define MOD_MAGIC_3 0x44u /* 'D' */

#define MOD_HEADER_SIZE      16u
#define MOD_SEGMENT_SIZE     12u
#define MOD_ENDPOINT_SIZE    8u
#define MOD_DEPENDENCY_SIZE  4u
#define MOD_PUBLICATION_SIZE 20u

#define MOD_NO_ENTRY 0xFFFFu

/* Format maxima (the loader's tables and the packer agree). */
#define MOD_MAX_SEGMENTS     4u
#define MOD_MAX_ENDPOINTS    8u
#define MOD_MAX_DEPENDENCIES 8u
/* Publications per image; the nucleus publication table is smaller (the
 * loader refuses a set that overflows it, VFS_NS_PUB_MAX). */
#define MOD_MAX_PUBLICATIONS 12u

/* Boot manifest bound: the loader's per-image loop. */
#define MOD_MAX_IMAGES       8u

/* Segment roles.  At most one segment per role; `hot` is required. */
#define MOD_SEGMENT_HOT  1u
#define MOD_SEGMENT_BOOT 2u
#define MOD_SEGMENT_IO   3u
#define MOD_SEGMENT_BULK 4u

/* Endpoint flags: bit 0 is the re-entrancy declaration, the same bit the
 * endpoint table keeps (KOS_IPC_EP_REENTRANT, abi/kos/ipc.h); bit 1 asks
 * the nucleus to tell the endpoint when a handle on one of its objects is
 * released (KOS_OP_RELEASE), so a driver that owns restoreable hardware
 * state can take the device back; bit 2 declares device events (the
 * manifest's `"events": true`): SYS_NOTIFY may subscribe to the endpoint's
 * events, which arrive as KOS_OP_SUBSCRIBE, and the driver posts them with
 * the bootstrap interface's event_post (architecture 4.5/4.10). */
#define MOD_EP_REENTRANT KOS_IPC_EP_REENTRANT
#define MOD_EP_RELEASE   0x02u
#define MOD_EP_EVENTS    0x04u

/* Container header, 16 bytes, little-endian.  `payload_offset` is the
 * first payload byte and equals the end of the tables; `total_size` is
 * the exact container length (the tables and the payload end there). */
struct mod_header {
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
 * boot role may carry one) or MOD_NO_ENTRY. */
struct mod_segment {
	uint8_t  role;            /* MOD_SEGMENT_*                    */
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
struct mod_endpoint {
	uint8_t  class;
	uint8_t  instance;
	uint8_t  version;
	uint8_t  flags;           /* MOD_EP_*                         */
	uint16_t number;
	uint16_t dispatch;
};

/* One resolved dependency, 4 bytes.  The host resolved the declared
 * (class, instance) to `number`, the endpoint number of an earlier (or
 * this image's own) export; 0 means the declaration was optional and no
 * image in the build exports it. */
struct mod_dependency {
	uint8_t  class;
	uint8_t  instance;
	uint16_t number;
};

/* One publication, 20 bytes.  `name` is the provider-relative namespace
 * name below the DEVICE provider's `/dev` root (`"/console"`),
 * NUL-terminated and zero-padded; `type` is the 8-bit protocol type
 * (abi/kos/object.h KOS_TYPE_*); `number` is the serving endpoint's
 * host-assigned number; `object` is the provider's own object identity,
 * which a nucleus provider receives with every call to the publication.
 * A host build never publishes a grouping name: the kernel derives the
 * container rows (`/input`, `/serial`) from the names themselves. */
struct mod_publication {
	char     name[16];        /* "/console", NUL-terminated */
	uint8_t  type;            /* KOS_TYPE_*                */
	uint8_t  number;          /* serving endpoint number   */
	uint8_t  object;          /* provider object identity  */
	uint8_t  flags;           /* 0                         */
};

#if defined(__SDCC)
_Static_assert(sizeof(struct mod_header) == MOD_HEADER_SIZE,
	       "mod header must be 16 bytes");
_Static_assert(__builtin_offsetof(struct mod_header, segment_count) == 4,
	       "mod header segment_count offset");
_Static_assert(__builtin_offsetof(struct mod_header, init_entry) == 8,
	       "mod header init_entry offset");
_Static_assert(__builtin_offsetof(struct mod_header, payload_offset) == 10,
	       "mod header payload_offset offset");
_Static_assert(sizeof(struct mod_segment) == MOD_SEGMENT_SIZE,
	       "mod segment must be 12 bytes");
_Static_assert(__builtin_offsetof(struct mod_segment, base) == 2,
	       "mod segment base offset");
_Static_assert(__builtin_offsetof(struct mod_segment, entry) == 8,
	       "mod segment entry offset");
_Static_assert(sizeof(struct mod_endpoint) == MOD_ENDPOINT_SIZE,
	       "mod endpoint must be 8 bytes");
_Static_assert(__builtin_offsetof(struct mod_endpoint, number) == 4,
	       "mod endpoint number offset");
_Static_assert(__builtin_offsetof(struct mod_endpoint, dispatch) == 6,
	       "mod endpoint dispatch offset");
_Static_assert(sizeof(struct mod_dependency) == MOD_DEPENDENCY_SIZE,
	       "mod dependency must be 4 bytes");
_Static_assert(sizeof(struct mod_publication) == MOD_PUBLICATION_SIZE,
	       "mod publication must be 20 bytes");
#endif /* __SDCC */

/* Loader error codes (0 = success, negative = failure). */
#define MOD_OK       0
#define MOD_EINVAL   (-KOS_EINVAL)  /* invalid argument/field value       */
#define MOD_EFORMAT  (-KOS_EIO)     /* corrupt/truncated/overlapping data */
#define MOD_ENOTSUP  (-KOS_ENOTSUP) /* unsupported version/type/reloc     */
#define MOD_ENOSPC   (-KOS_ENOSPC)  /* table or publication exhaustion    */
#define MOD_ENOENT   (-KOS_ENOENT)  /* no such manifest/file              */
#define MOD_EBUSY    (-KOS_EBUSY)   /* nucleus table refused the record   */

/* The flat ROMFS name of the boot manifest.  The bootstrap reader
 * (kernel/romfs) resolves it like any other flat name, without path
 * syntax; every read is staged through common memory. */
#define KOS_DRIVER_MANIFEST_NAME "drivers.man"

/* Manifest record: exactly the ROMFS name field, NUL-terminated and
 * zero-padded.  The manifest file's size is a multiple of this. */
#define KOS_DRIVER_MANIFEST_ENTRY ROMFS_NAME_SIZE

/* --- the resident loader (kernel/driver/driver_loader.c) --------------
 *
 * Cold nucleus code: read the boot manifest and bring up every driver
 * image it names, in load order.  Returns 0 or a negative MOD/KOS error;
 * a failure is a boot error and is reported on the early console.
 * Drivers are permanent, so a second call is
 * a no-op. */
int driver_load_all(const char *manifest_name);

#endif /* KONAOS_DRIVER_H */
