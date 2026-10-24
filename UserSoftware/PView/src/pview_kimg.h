/* PView: the KIMG v1 header validation, shared by the viewer and its
 * host test.
 *
 * The validator works on the raw 16 bytes (the header is little-endian
 * and packed, and the host's C does not share SDCC's struct layout):
 * magic, version, flags, the mode, its geometry and the exact payload
 * size a picture of width x height holds in that mode's packing.
 */

#ifndef PVIEW_KIMG_H
#define PVIEW_KIMG_H

#include "kos/types.h"

/* enum gfx_mode values, spelled out for the header byte. */
#define PVIEW_MODE_G6  0u
#define PVIEW_MODE_G7  1u
#define PVIEW_MODE_YJK 2u

/* Validate the 16-byte KIMG v1 header at `raw`: 0 when the viewer can
 * display it, or -KOS_EINVAL.  Touches nothing but the argument. */
int pview_kimg_check(const uint8_t *raw);

#endif /* PVIEW_KIMG_H */
