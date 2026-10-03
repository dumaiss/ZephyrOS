/* Internal interface of the V9958 video driver (drivers/v9958/).
 *
 * v9958.c owns the GRAPHIC 6 display state, the ported initialization
 * and command sequencing, and the font-atlas upload; v9958_io.asm owns
 * every port access and the timing-critical streaming loops;
 * v9958_module.c owns the zdevice descriptor and the module init entry.
 * The module reaches the kernel only through the passed `zdriver_api`
 * (resource imports for the ROMFS font; no imported kernel symbol and
 * no direct ROMFS access).  Not a kernel ABI.
 *
 * The ported behavior is the hardware-proven 85x26, 6x8 CP850 GRAPHIC 6
 * bring-up of the pinned Zephyr-80 reference (LunchCrema V9958,
 * commit 2c6b9650): software-paced bootstrap with the front porch
 * bypassed, native WAIT from R#25.WTE=1 on, palette programmed only
 * after the porch is enabled, both precolored font atlases uploaded
 * from the reference font, circular page-zero bitmap cleared, sprite
 * cursor initialized, then the display enabled.  The VDP interrupt is
 * never enabled in M8.
 */

#ifndef ZEPHYROS_DRIVERS_V9958_DRIVER_H
#define ZEPHYROS_DRIVERS_V9958_DRIVER_H

#include "zephyros/device.h"
#include "zephyros/driver.h"
#include "zephyros/video.h"

/* --- GRAPHIC 6 layout (hardware-proven 85x26 console baseline) ---------
 *
 * 512x256-byte page-zero bitmap (256 scanlines of one 256-byte G6 pitch),
 * 85 columns of 6-pixel cells, 26 rows of 8-line cells, display offset
 * 4 source lines.  Only the layout constants the core owns in package
 * 18 are declared here; the character renderer/terminal arrives with the
 * console package. */

#define V9958_ATLAS_SCANLINES   64u  /* 8 atlas rows x 8 glyph lines      */
#define V9958_ATLAS_ROW_BYTES   96u  /* 32 glyphs x 6 pixels / 2 per byte */
#define V9958_ATLAS_PITCH       256u /* one G6 scanline (512 px, 4bpp)    */
#define V9958_ATLAS_GLYPHS      32u  /* glyphs per precolored atlas row   */
#define V9958_ATLAS_GROUPS      8u   /* 256 glyphs / 32 per group         */
#define V9958_GLYPH_BYTES       8u   /* 6x8 font: 8 rows per glyph        */

/* Atlas placement in 128-KiB VRAM: normal at 10000h, reverse at 14000h,
 * each scanline one 256-byte pitch apart (96 meaningful bytes each). */
#define V9958_ATLAS_NORMAL_A16  1u
#define V9958_ATLAS_NORMAL_ADDR 0x0000u
#define V9958_ATLAS_REVERSE_A16 1u
#define V9958_ATLAS_REVERSE_ADDR 0x4000u

/* Font resource carried as ROMFS DATA (never resident module storage). */
#define V9958_FONT_NAME   "v9958-6x8.font"
#define V9958_FONT_SIZE   2048u
#define V9958_FONT_CHUNK  256u  /* one 32-glyph group per bounded read */

/* --- Sprite-mode-2 cursor allocations (SAT + pattern + color) --------- */

#define V9958_CURSOR_PATTERN_A16 1u
#define V9958_CURSOR_PATTERN_ADDR 0xF800u
#define V9958_CURSOR_COLOR_A16   1u
#define V9958_CURSOR_COLOR_ADDR  0xF000u
#define V9958_CURSOR_SAT_A16     1u
#define V9958_CURSOR_SAT_ADDR    0xF200u
#define V9958_CURSOR_COLOR_INDEX 0x0Bu /* yellow, proven on the card     */
#define V9958_CURSOR_TERMINATOR  0xD8u /* hidden sprite Y terminator     */
#define V9958_CURSOR_PATTERN_ROW 0xE0u /* six-pixel top bar glyph row    */
#define V9958_CURSOR_COLORS      16u
#define V9958_CURSOR_SAT_BYTES   8u

/* --- Command-engine register block (R#32..R#46 via the indirect port) -
 *
 * Offsets inside the 15-byte block; R#46 is last and starts the command.
 * HMMV = fill rectangle, HMMM = copy rectangle.  These are the command
 * codes used by the reference renderer and clear/cursor setup. */

#define V9958_CMD_SX_LO  0u
#define V9958_CMD_SX_HI  1u
#define V9958_CMD_SY_LO  2u
#define V9958_CMD_SY_HI  3u
#define V9958_CMD_DX_LO  4u
#define V9958_CMD_DX_HI  5u
#define V9958_CMD_DY_LO  6u
#define V9958_CMD_DY_HI  7u
#define V9958_CMD_NX_LO  8u
#define V9958_CMD_NX_HI  9u
#define V9958_CMD_NY_LO  10u
#define V9958_CMD_NY_HI  11u
#define V9958_CMD_COLOR  12u
#define V9958_CMD_ARG    13u
#define V9958_CMD_CODE   14u

#define V9958_CMD_HMMV   0xC0u
#define V9958_CMD_HMMM   0xD0u

/* Clear-screen fill: the full 256-line page-zero bitmap in the default
 * background (palette index 4, packed byte 44h). */
#define V9958_CLEAR_WIDTH_LO  0x00u
#define V9958_CLEAR_WIDTH_HI  0x02u /* 512 pixels                          */
#define V9958_CLEAR_HEIGHT_LO 0x00u
#define V9958_CLEAR_HEIGHT_HI 0x01u /* 256 lines                           */
#define V9958_CLEAR_COLOR     0x44u

/* --- Port-call ABI with v9958_io.asm ----------------------------------
 *
 * All single-byte port writes are issued by the stream primitives; the
 * caller stages its byte in this block.  The assembler consumes
 * `src` at offsets 0..1 and `length` (little-endian, 1..65535) at
 * offsets 2..3; the layout is part of the driver-private ABI. */

struct v9958_block {
	const uint8_t *src;
	uint16_t length;
};

#if defined(__SDCC)
_Static_assert(sizeof(struct v9958_block) == 4,
	       "v9958_block must be 4 packed bytes");
_Static_assert(__builtin_offsetof(struct v9958_block, length) == 2,
	       "v9958_block length offset");
#endif /* __SDCC */

/* v9958_io.asm entry points. */
void v9958_io_bootstrap_delay(void);
void v9958_io_reg_paced(uint8_t value, uint8_t reg);
void v9958_io_reg(uint8_t value, uint8_t reg);
uint8_t v9958_io_status_read(void);
void v9958_io_wait_command(void);
void v9958_io_latch_write(uint8_t value);
void v9958_io_stream_ctrl(struct v9958_block *block);
void v9958_io_stream_data(struct v9958_block *block);
void v9958_io_stream_palette(struct v9958_block *block);
void v9958_io_stream_indirect(struct v9958_block *block);

/* Driver state: module BSS, relocated with the module image. */
struct v9958_state {
	const struct zdriver_api *api; /* resource imports for the font   */
	uint8_t config_shadow;         /* latch shadow; D0 always preserved */
	uint8_t scroll_origin;         /* physical scanline of row zero    */
};

/* Cold bring-up (full initialization incl. the font upload).  Returns
 * ZDRIVER_OK or ZDRIVER_FAIL; registering the device stays with the
 * module entry so a failure publishes nothing. */
int v9958_driver_init(const struct zdriver_api *api);

/* The class operation implementation (exposed for the descriptor). */
int v9958_send(void *state, uint8_t operation, const void *payload,
	       uint16_t length);

extern const struct zvideo_ops v9958_video_ops;
extern struct v9958_state v9958_state;

#endif /* ZEPHYROS_DRIVERS_V9958_DRIVER_H */
