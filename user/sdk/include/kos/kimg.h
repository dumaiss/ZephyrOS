/* KonaOS SDK: the KIMG v1 simple image format (docs/gfx-sdk.md
 * section 16).
 *
 * A host-prepared image: a 16-byte header, then `palette_count` RGB
 * triples, then the pixel payload in the mode's packing.  All
 * multibyte fields are little-endian.  The host converter is
 * tools/kimg.py; the target decodes nothing.
 *
 *   offset size field
 *   00     4   magic = "KIMG"
 *   04     1   version = 1
 *   05     1   mode (the enum gfx_mode value)
 *   06     1   flags = 0
 *   07     1   palette_count (RGB triples before the payload)
 *   08     2   width
 *   0A     2   height
 *   0C     4   payload_size
 *
 * Payload packings: the 16-colour mode two 4-bit pixels per byte, the
 * 256-colour mode one pixel per byte, the YJK mode one host-encoded
 * YJK byte per pixel.
 */

#ifndef KOS_SDK_KIMG_H
#define KOS_SDK_KIMG_H

#include "kos/types.h"

#define KIMG_MAGIC_0 'K'
#define KIMG_MAGIC_1 'I'
#define KIMG_MAGIC_2 'M'
#define KIMG_MAGIC_3 'G'

#define KIMG_VERSION    1u
#define KIMG_FLAGS      0u
#define KIMG_HEADER_SIZE 16u

struct kimg_header {
	uint8_t magic[4];        /* "KIMG"                   */
	uint8_t version;         /* KIMG_VERSION             */
	uint8_t mode;            /* the enum gfx_mode value  */
	uint8_t flags;           /* 0                        */
	uint8_t palette_count;   /* RGB triples that follow  */
	uint16_t width;
	uint16_t height;
	uint32_t payload_size;
};

#if defined(__SDCC)
_Static_assert(sizeof(struct kimg_header) == KIMG_HEADER_SIZE,
	       "kimg_header must be 16 packed bytes");
_Static_assert(__builtin_offsetof(struct kimg_header, version) == 4 &&
	       __builtin_offsetof(struct kimg_header, mode) == 5 &&
	       __builtin_offsetof(struct kimg_header, palette_count) == 7 &&
	       __builtin_offsetof(struct kimg_header, width) == 8 &&
	       __builtin_offsetof(struct kimg_header, height) == 10 &&
	       __builtin_offsetof(struct kimg_header, payload_size) == 12,
	       "kimg_header layout");
#endif /* __SDCC */

#endif /* KOS_SDK_KIMG_H */
