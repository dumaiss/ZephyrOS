/* KRES host-build shim for the native ABI's kos/types.h.
 *
 * The real header (abi/kos/types.h) defines the fixed-width types in
 * terms of SDCC's C types, where long is 32 bits; on a 64-bit host
 * that would widen uint32_t to 64 bits and break the on-disk layout.
 * Host builds of the KRES core put this directory on the include path
 * ahead of abi/, so the layout is identical on both platforms.  The
 * SDCC build sees the real header.
 */

#ifndef KOS_TYPES_H
#define KOS_TYPES_H

#include <stdint.h>

#endif /* KOS_TYPES_H */
