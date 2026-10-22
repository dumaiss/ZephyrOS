/* KonaOS IPC acceptance: the `echo` driver's mapped segment (test
 * artifact).
 *
 * A known pattern (the const area, the segment's image) and a scratch
 * area (the data area, the segment's BSS), both placed by the planner in
 * bank 6's mapped range 0100h-1FFFh (architecture 5.2).  The driver reads
 * and writes them at their linked addresses only inside a bulk-map span.
 */

#include "kos/types.h"

#include "ipc_test.h"

const uint8_t echo_mapped[IPC_SPAN_BYTES] = {
	0xC0u, 0xC1u, 0xC2u, 0xC3u, 0xC4u, 0xC5u, 0xC6u, 0xC7u,
	0xC8u, 0xC9u, 0xCAu, 0xCBu, 0xCCu, 0xCDu, 0xCEu, 0xCFu,
	0xD0u, 0xD1u, 0xD2u, 0xD3u, 0xD4u, 0xD5u, 0xD6u, 0xD7u,
	0xD8u, 0xD9u, 0xDAu, 0xDBu, 0xDCu, 0xDDu, 0xDEu, 0xDFu,
};

uint8_t echo_mapped_bss[IPC_SPAN_BYTES];
