/* Host stand-ins: the exact-width types the SDK's kos/reader.h expects.
 * The target build takes these from abi/kos/types.h; the host twin needs
 * genuine 16-bit arithmetic (the CRC relies on it), so stdint.h. */

#ifndef KOS_TYPES_H
#define KOS_TYPES_H

#include <stdint.h>

#endif
