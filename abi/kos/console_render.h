/* KonaOS console render transaction ABI.
 *
 * One descriptor contains one validated stream.  Records use absolute
 * 0-based cells and never expose a display controller command.  INVERT keeps
 * fallback text so a non-bitmap backend can repaint the same result.
 */

#ifndef KOS_CONSOLE_RENDER_H
#define KOS_CONSOLE_RENDER_H

#include "kos/types.h"

#define KCONSOLE_RENDER_VERSION 1u

#define KCONSOLE_RENDER_TEXT   1u
#define KCONSOLE_RENDER_INVERT 2u
#define KCONSOLE_RENDER_STATE  3u

#define KCONSOLE_RENDER_HEADER_SIZE 4u
#define KCONSOLE_RENDER_SPAN_BASE   6u
#define KCONSOLE_RENDER_STATE_SIZE  6u

#define KCONSOLE_RENDER_COLS 85u
#define KCONSOLE_RENDER_ROWS 26u

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
