/* KonaOS console render transaction and grid ABI.
 *
 * One descriptor contains one validated stream.  Records use absolute
 * 0-based cells inside the grid the console reports (KCONSOLE_OP_INFO,
 * kos/protocol.h) and never expose a display controller command.  INVERT keeps
 * fallback text so a non-bitmap backend can repaint the same result.
 */

#ifndef KOS_CONSOLE_RENDER_H
#define KOS_CONSOLE_RENDER_H

#include "kos/types.h"

#define KCONSOLE_RENDER_VERSION 1u

#define KCONSOLE_RENDER_TEXT   1u
#define KCONSOLE_RENDER_INVERT 2u
#define KCONSOLE_RENDER_STATE  3u

/* The console's grid (KCONSOLE_OP_INFO and KVIDEO_OP_INFO,
 * kos/protocol.h): struct kconsole_info's attr_bits and flags. */
#define KCONSOLE_ATTR_REVERSE 0x01u /* attr bit 0: reverse video         */
#define KCONSOLE_INFO_RENDER  0x01u /* KCONSOLE_OP_RENDER is served      */

/* KCONSOLE_OP_INFO's (and KVIDEO_OP_INFO's) reply payload, the first
 * KCONSOLE_INFO_SIZE bytes of the frame payload. */
#define KCONSOLE_INFO_SIZE 8u
struct kconsole_info {
	uint8_t cols;        /* text columns                             */
	uint8_t rows;        /* text rows                                */
	uint8_t attr_bits;   /* KCONSOLE_ATTR_* a render record may use  */
	uint8_t flags;       /* KCONSOLE_INFO_*                          */
	uint8_t reserved[4]; /* zero                                     */
};
#if defined(__SDCC)
_Static_assert(sizeof(struct kconsole_info) == KCONSOLE_INFO_SIZE,
	       "kconsole_info size");
#endif

#define KCONSOLE_RENDER_HEADER_SIZE 4u
#define KCONSOLE_RENDER_SPAN_BASE   6u
#define KCONSOLE_RENDER_STATE_SIZE  6u

struct kconsole_render_header {
	uint8_t version;
	uint8_t flags;
	uint16_t total_length;
};

struct kconsole_render_span {
	uint8_t op;
	uint8_t record_length;
	uint8_t x;
	uint8_t y;
	uint8_t attr;
	uint8_t count;
};

struct kconsole_render_state {
	uint8_t op;
	uint8_t record_length;
	uint8_t x;
	uint8_t y;
	uint8_t attr;
	uint8_t visible;
};

#if defined(__SDCC)
_Static_assert(sizeof(struct kconsole_render_header) ==
	       KCONSOLE_RENDER_HEADER_SIZE, "render header size");
_Static_assert(sizeof(struct kconsole_render_span) ==
	       KCONSOLE_RENDER_SPAN_BASE, "render span size");
_Static_assert(sizeof(struct kconsole_render_state) ==
	       KCONSOLE_RENDER_STATE_SIZE, "render state size");
#endif

#endif /* KOS_CONSOLE_RENDER_H */
