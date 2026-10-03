/* ZephyrOS V9958 video driver implementation (drivers/v9958/).
 *
 * This is the direct-hardware core ported from the pinned Zephyr-80
 * reference (LunchCrema V9958, commit 2c6b9650): the exact GRAPHIC 6
 * initialization discipline, the native VIDEO class operation surface,
 * the ROMFS-backed 6x8 CP850 font-atlas setup, and the low-level
 * graphics/command primitives the renderer needs.  It deliberately does
 * not yet advertise character/console output (package 19 adds the
 * renderer and `zchar_ops`).
 *
 * Initialization discipline (hardware-validated; never reordered):
 *
 *   1. preserve D0 in the latch shadow, disable the U11 porch (D1=1),
 *      and software-pace every bootstrap access;
 *   2. R#25 = 00h (WTE=0, VDS=0), R#8 = 08h (64Kx4 DRAM organization),
 *      stop any prior command, program the full display/selector state;
 *   3. R#25 = 04h (WTE=1, VDS=0), then enable the porch (D1=0);
 *   4. program the palette only here -- every byte under a real
 *      hardware WAIT (WTE=0 loading corrupted palette low bits on the
 *      physical card);
 *   5. upload the normal and reverse font atlases from the ROMFS font,
 *      clear G6, initialize the sprite cursor, present, and finally
 *      enable the display.
 *
 * The font is 2048 bytes of ROMFS DATA read during init through the
 * ABI-v2 resource imports in bounded 256-byte chunks: it is never
 * embedded in the module image or BSS, and there is no build-time
 * dependency on the adjacent reference repository.
 *
 * VDP interrupts stay disabled for all of M8: nothing here selects an
 * interrupt enable, so the M2 IRQ_VDP_FF path is not claimed.
 *
 * This source is compiled into the v9958 ZMOD module; the buffers and
 * state below live in module BSS (`_DATA`) and are relocated with the
 * module image.  The module reaches the kernel only through the passed
 * `zdriver_api`.
 */

#include "zephyros/platform.h"
#include "zephyros/device.h"
#include "zephyros/driver.h"
#include "zephyros/video.h"

#include "v9958_driver.h"

struct v9958_state v9958_state;

/* Bounded scratch (module BSS, reused for every read/atlas row):
 * one 32-glyph font group and one expanded 96-byte atlas scanline.
 * The reverse flag mirrors the reference driver: the second pass swaps
 * foreground/background in the pair table. */
static uint8_t v9958_font_group[V9958_FONT_CHUNK];
static uint8_t v9958_atlas_row[V9958_ATLAS_ROW_BYTES];
static uint8_t v9958_cmd[ZOS_V9958_CMD_REGISTER_COUNT];
static uint8_t v9958_atlas_reverse;

/* Real-card G6 baseline from the verified bring-up sequence (R#0..R#11);
 * R#1 deliberately keeps the display disabled until VRAM setup ends. */
static const uint8_t v9958_g6_registers[12] = {
	ZOS_V9958_R0_G6,             /* R#0: M5+M3 -> GRAPHIC 6          */
	ZOS_V9958_R1_DISPLAY_OFF,    /* R#1: display off during setup   */
	ZOS_V9958_R2_G6,             /* R#2: page-zero baseline         */
	0x00u,                       /* R#3                             */
	0x00u,                       /* R#4                             */
	ZOS_V9958_R5_SPRITE_BASE,    /* R#5/R#11 -> color/SAT bases     */
	ZOS_V9958_R6_SPRITE_PATTERN, /* R#6: sprite pattern table       */
	ZOS_V9958_R7_BORDER,         /* R#7: text/backdrop color        */
	ZOS_V9958_R8_64K_DRAM,       /* R#8: VR=1, 64Kx4 DRAMs          */
	ZOS_V9958_R9_212_INTERLACE,  /* R#9: 212 lines, interlace       */
	0x00u,                       /* R#10                            */
	ZOS_V9958_R11_SPRITE_ATTR    /* R#11                            */
};

/* Palette entries 0..15, encoded as (R<<4)|B then G.  Index 0 black,
 * 4 blue, 15 white; the console renderer uses those. */
static const uint8_t v9958_palette[32] = {
	0x00u, 0x00u, 0x11u, 0x01u, 0x00u, 0x06u, 0x00u, 0x07u,
	0x05u, 0x00u, 0x07u, 0x03u, 0x50u, 0x00u, 0x06u, 0x06u,
	0x70u, 0x00u, 0x73u, 0x03u, 0x70u, 0x07u, 0x74u, 0x07u,
	0x00u, 0x05u, 0x67u, 0x00u, 0x55u, 0x05u, 0x77u, 0x07u
};

/* Sprite cursor assets and their precolored pair table.  The cursor
 * pattern is the six-pixel top bar of a text cell. */
static const uint8_t v9958_cursor_pattern[8] = {
	0xE0u, 0xE0u, 0xE0u, 0xE0u, 0xE0u, 0xE0u, 0xE0u, 0xE0u
};
static const uint8_t v9958_cursor_colors[V9958_CURSOR_COLORS] = {
	0x0Bu, 0x0Bu, 0x0Bu, 0x0Bu, 0x0Bu, 0x0Bu, 0x0Bu, 0x0Bu,
	0x0Bu, 0x0Bu, 0x0Bu, 0x0Bu, 0x0Bu, 0x0Bu, 0x0Bu, 0x0Bu
};
static const uint8_t v9958_pair_normal[4] = {
	0x44u, 0x4Fu, 0xF4u, 0xFFu
};
static const uint8_t v9958_pair_reverse[4] = {
	0xFFu, 0xF4u, 0x4Fu, 0x44u
};

/* --- low-level helpers -------------------------------------------------- */

static void v9958_write_ctrl_byte(uint8_t value)
{
	struct v9958_block block;

	block.src = &value;
	block.length = 1u;
	v9958_io_stream_ctrl(&block);
}

static void v9958_write_palette_byte(uint8_t value)
{
	struct v9958_block block;

	block.src = &value;
	block.length = 1u;
	v9958_io_stream_palette(&block);
}

/* Change only /WS_EN (D1); D0 stays the selected interrupt route held in
 * the software shadow because the lunch latch captures both bits
 * together.  Porch off is software paced; porch on runs under native
 * WAIT. */
static void v9958_porch_off(void)
{
	uint8_t value = (uint8_t)((v9958_state.config_shadow &
				   ZOS_V9958_CONFIG_INT_ROUTE) |
				  ZOS_V9958_CONFIG_PORCH_OFF);

	v9958_state.config_shadow = value;
	v9958_io_latch_write(value);
	v9958_io_bootstrap_delay();
}

static void v9958_porch_on(void)
{
	uint8_t value = (uint8_t)(v9958_state.config_shadow &
				  ZOS_V9958_CONFIG_INT_ROUTE);

	v9958_state.config_shadow = value;
	v9958_io_latch_write(value);
}

/* Write one precolored atlases row / cursor block.  `addr` is the low
 * 16 address bits; `a16` is bit 16.  R#14 carries A16..A14, then the
 * two command-port address bytes select the VRAM destination and the
 * data stream follows.  A prior command always completes first. */
static void v9958_write_vram(const uint8_t *src, uint16_t addr, uint8_t a16,
			     uint8_t count)
{
	uint8_t r14 = (uint8_t)(((a16 & 1u) << 2) |
				((uint8_t)(addr >> 14) & 0x03u));
	uint8_t lo = (uint8_t)(addr & 0xFFu);
	uint8_t hi = (uint8_t)(((uint8_t)(addr >> 8) & 0x3Fu) | 0x40u);
	struct v9958_block block;

	v9958_io_wait_command();
	v9958_io_reg(r14, ZOS_V9958_REG_R14);
	block.src = &lo;
	block.length = 1u;
	v9958_io_stream_ctrl(&block);
	block.src = &hi;
	v9958_io_stream_ctrl(&block);
	block.src = src;
	block.length = count;
	v9958_io_stream_data(&block);
}

/* R#32..R#46 are written in order through the indirect port; R#46 is
 * last and starts the command.  A prior command always finishes first. */
static void v9958_start_command(const uint8_t *cmd)
{
	struct v9958_block block;

	v9958_io_wait_command();
	v9958_io_reg(ZOS_V9958_CMD_REGISTER_BASE, ZOS_V9958_REG_R17);
	block.src = cmd;
	block.length = ZOS_V9958_CMD_REGISTER_COUNT;
	v9958_io_stream_indirect(&block);
}

/* Convert one font scanline byte (bits 7..2 = six pixels) into three
 * precolored G6 bytes.  The reverse pass swaps foreground/background so
 * SGR reverse video stays a single glyph copy. */
static void v9958_expand_row(uint8_t bits, uint8_t *dst)
{
	const uint8_t *table = (v9958_atlas_reverse != 0u) ?
		v9958_pair_reverse : v9958_pair_normal;

	dst[0] = table[(uint8_t)((bits >> 6) & 0x03u)];
	dst[1] = table[(uint8_t)((bits >> 4) & 0x03u)];
	dst[2] = table[(uint8_t)((bits >> 2) & 0x03u)];
}

/* --- display state ------------------------------------------------------ */

/* Step 2: the non-negotiable hardware state before any accelerated
 * access.  The display stays off until the bitmap, atlases and cursor
 * are initialized. */
static void v9958_init_g6(void)
{
	uint8_t i;

	v9958_porch_off();
	v9958_state.scroll_origin = 0u;

	v9958_io_reg_paced(ZOS_V9958_R25_WAIT_OFF, ZOS_V9958_REG_R25);
	v9958_io_reg_paced(ZOS_V9958_R8_64K_DRAM, ZOS_V9958_REG_R8);

	/* A warm boot may follow a program that left a command active. */
	v9958_io_reg_paced(0x00u, ZOS_V9958_REG_R46);

	for (i = 0u; i < 12u; ++i) {
		v9958_io_reg_paced(v9958_g6_registers[i], i);
	}

	/* Explicitly establish every selector/latch used by later
	 * helpers; R#26/R#27 are horizontal-scroll state that survives a
	 * warm boot. */
	v9958_io_reg_paced(0x00u, ZOS_V9958_REG_R14);
	v9958_io_reg_paced(ZOS_V9958_R15_STATUS2, ZOS_V9958_REG_R15);
	v9958_io_reg_paced(0x00u, ZOS_V9958_REG_R16);
	v9958_io_reg_paced(0x00u, ZOS_V9958_REG_R17);
	v9958_io_reg_paced(0x00u, ZOS_V9958_REG_R18);
	v9958_io_reg_paced(ZOS_V9958_R23_TEXT_BASE, ZOS_V9958_REG_R23);
	v9958_io_reg_paced(0x00u, ZOS_V9958_REG_R26);
	v9958_io_reg_paced(0x00u, ZOS_V9958_REG_R27);
}

/* Step 3: enable native WAIT first (WTE=1, VDS stays 0 so pin 8 keeps
 * supplying CPUCLK), then enable the U11 front porch. */
static void v9958_enable_hardware_wait(void)
{
	v9958_io_reg_paced(ZOS_V9958_R25_WAIT_ON, ZOS_V9958_REG_R25);
	v9958_porch_on();
}

/* Step 4: palette entries 0..15, each explicitly selected; every byte
 * is held by a real hardware WAIT, exactly as proven on the card.  The
 * software pacing is retained because it is harmless here. */
static void v9958_init_palette(void)
{
	uint8_t i;

	for (i = 0u; i < 16u; ++i) {
		v9958_io_reg_paced(i, ZOS_V9958_REG_R16);
		v9958_write_palette_byte(v9958_palette[(uint16_t)i * 2u]);
		v9958_io_bootstrap_delay();
		v9958_write_palette_byte(
			v9958_palette[(uint16_t)i * 2u + 1u]);
		v9958_io_bootstrap_delay();
	}
}

/* Convert one 32-glyph group of the resident font into eight 96-byte
 * atlas scanlines and stream them to VRAM. */
static int v9958_upload_atlas_group(uint8_t group, uint8_t reverse)
{
	uint8_t scanline;
	uint8_t i;
	uint16_t addr;
	int rc;

	rc = v9958_state.api->resource_read(
		V9958_FONT_NAME,
		(uint16_t)((uint16_t)group * V9958_FONT_CHUNK),
		v9958_font_group, V9958_FONT_CHUNK);
	if (rc != (int)V9958_FONT_CHUNK) {
		return ZDRIVER_FAIL;
	}

	if (reverse != 0u) {
		v9958_atlas_reverse = 1u;
	} else {
		v9958_atlas_reverse = 0u;
	}
	for (scanline = 0u; scanline < V9958_GLYPH_BYTES; ++scanline) {
		uint8_t s = (uint8_t)((uint8_t)(group << 3) | scanline);
		uint8_t *dst = &v9958_atlas_row[0];

		for (i = 0u; i < V9958_ATLAS_GLYPHS; ++i) {
			v9958_expand_row(
				v9958_font_group[(uint16_t)((uint16_t)i *
							    V9958_GLYPH_BYTES) +
						 scanline],
				dst);
			dst += 3u;
		}
		addr = (uint16_t)((uint16_t)s * V9958_ATLAS_PITCH);
		if (reverse != 0u) {
			addr = (uint16_t)(addr + V9958_ATLAS_REVERSE_ADDR);
			v9958_write_vram(v9958_atlas_row, addr,
					 V9958_ATLAS_REVERSE_A16,
					 V9958_ATLAS_ROW_BYTES);
		} else {
			addr = (uint16_t)(addr + V9958_ATLAS_NORMAL_ADDR);
			v9958_write_vram(v9958_atlas_row, addr,
					 V9958_ATLAS_NORMAL_A16,
					 V9958_ATLAS_ROW_BYTES);
		}
	}
	return ZDRIVER_OK;
}

/* Step 5a: the exact 2048-byte font is streamed through the resource
 * imports in bounded 256-byte chunks.  The size is validated up front
 * (before any port I/O) so a missing or wrong-sized resource cannot
 * leave the VDP half-initialized. */
static int v9958_upload_font_atlas(void)
{
	uint8_t group;

	for (group = 0u; group < V9958_ATLAS_GROUPS; ++group) {
		if (v9958_upload_atlas_group(group, 0u) != ZDRIVER_OK) {
			return ZDRIVER_FAIL;
		}
	}
	for (group = 0u; group < V9958_ATLAS_GROUPS; ++group) {
		if (v9958_upload_atlas_group(group, 1u) != ZDRIVER_OK) {
			return ZDRIVER_FAIL;
		}
	}
	return ZDRIVER_OK;
}

/* Step 5b: clear all 256 lines of the circular page-zero bitmap and
 * restore its origin. */
static void v9958_clear_screen(void)
{
	uint8_t i;

	v9958_state.scroll_origin = 0u;
	v9958_io_reg(ZOS_V9958_R23_TEXT_BASE, ZOS_V9958_REG_R23);
	for (i = 0u; i < ZOS_V9958_CMD_REGISTER_COUNT; ++i) {
		v9958_cmd[i] = 0x00u;
	}
	v9958_cmd[V9958_CMD_NX_LO] = V9958_CLEAR_WIDTH_LO;
	v9958_cmd[V9958_CMD_NX_HI] = V9958_CLEAR_WIDTH_HI;
	v9958_cmd[V9958_CMD_NY_LO] = V9958_CLEAR_HEIGHT_LO;
	v9958_cmd[V9958_CMD_NY_HI] = V9958_CLEAR_HEIGHT_HI;
	v9958_cmd[V9958_CMD_COLOR] = V9958_CLEAR_COLOR;
	v9958_cmd[V9958_CMD_CODE] = V9958_CMD_HMMV;
	v9958_start_command(v9958_cmd);
	v9958_io_wait_command();
}

/* Step 5c: sprite cursor -- pattern, color table and SAT.  Logical row
 * zero sits at scanline `scroll_origin`; the cursor SAT Y is one line
 * above the cell (80h+ when the origin is zero, as in the reference). */
static void v9958_cursor_init(void)
{
	uint8_t sat[V9958_CURSOR_SAT_BYTES];

	sat[0] = (uint8_t)(v9958_state.scroll_origin - 1u);
	sat[1] = 0x00u;
	sat[2] = 0x00u;
	sat[3] = 0x00u;
	sat[4] = V9958_CURSOR_TERMINATOR;
	sat[5] = 0x00u;
	sat[6] = 0x00u;
	sat[7] = 0x00u;

	v9958_write_vram(v9958_cursor_pattern, V9958_CURSOR_PATTERN_ADDR,
			 V9958_CURSOR_PATTERN_A16, 8u);
	v9958_write_vram(v9958_cursor_colors, V9958_CURSOR_COLOR_ADDR,
			 V9958_CURSOR_COLOR_A16, V9958_CURSOR_COLORS);
	v9958_write_vram(sat, V9958_CURSOR_SAT_ADDR, V9958_CURSOR_SAT_A16,
			 V9958_CURSOR_SAT_BYTES);
}

/* The complete bring-up in the proven order.  Re-establishing this on
 * the ZVIDEO_RESET operation is exactly the reference reset path: every
 * owned register, selector, palette entry and VRAM asset is rewritten,
 * the latch shadow (and therefore D0) is preserved, and the display is
 * left enabled. */
static int v9958_init_display(void)
{
	v9958_init_g6();
	v9958_enable_hardware_wait();
	v9958_init_palette();
	if (v9958_upload_font_atlas() != ZDRIVER_OK) {
		return ZDRIVER_FAIL;
	}
	v9958_clear_screen();
	v9958_cursor_init();
	v9958_io_wait_command(); /* present */
	v9958_io_reg(ZOS_V9958_R1_DISPLAY_ON, ZOS_V9958_REG_R1);
	return ZDRIVER_OK;
}

/* --- class operations --------------------------------------------------- */

int v9958_send(void *vstate, uint8_t operation, const void *payload,
	       uint16_t length)
{
	struct v9958_state *state = (struct v9958_state *)vstate;
	struct v9958_block block;

	if (state == (struct v9958_state *)0) {
		return ZDEV_ENODEV;
	}

	switch (operation) {
	case ZVIDEO_CTRL_WRITE:
	case ZVIDEO_DATA_WRITE:
	case ZVIDEO_DATA_BLOCK:
	case ZVIDEO_PALETTE_WRITE:
	case ZVIDEO_INDIRECT_WRITE:
		/* Validate before any I/O: a malformed request must cause
		 * no port writes.  The full 16-bit length is streamed. */
		if (payload == (const void *)0 || length == 0u) {
			return ZDEV_EINVAL;
		}
		block.src = (const uint8_t *)payload;
		block.length = length;
		switch (operation) {
		case ZVIDEO_CTRL_WRITE:
			v9958_io_stream_ctrl(&block);
			break;
		case ZVIDEO_DATA_WRITE:
		case ZVIDEO_DATA_BLOCK:
			v9958_io_stream_data(&block);
			break;
		case ZVIDEO_PALETTE_WRITE:
			v9958_io_stream_palette(&block);
			break;
		default:
			v9958_io_stream_indirect(&block);
			break;
		}
		return ZDEV_OK;
	case ZVIDEO_PRESENT:
		if (length != 0u) {
			return ZDEV_EINVAL;
		}
		v9958_io_wait_command();
		return ZDEV_OK;
	case ZVIDEO_RESET:
		if (length != 0u) {
			return ZDEV_EINVAL;
		}
		if (v9958_init_display() != ZDRIVER_OK) {
			return ZDEV_ENODEV;
		}
		return ZDEV_OK;
	default:
		return ZDEV_ENOTSUP;
	}
}

const struct zvideo_ops v9958_video_ops = {
	v9958_send
};

int v9958_driver_init(const struct zdriver_api *api)
{
	uint16_t size = 0u;

	if (api == (const struct zdriver_api *)0) {
		return ZDRIVER_FAIL;
	}
	if (api->resource_size ==
		    (int (*)(const char *, uint16_t *))0 ||
	    api->resource_read ==
		    (int (*)(const char *, uint16_t, void *,
			     uint16_t))0) {
		return ZDRIVER_FAIL;
	}
	/* Validate the font resource before any port I/O: a missing or
	 * wrong-sized ROMFS DATA entry must not leave the VDP
	 * half-initialized. */
	if (api->resource_size(V9958_FONT_NAME, &size) != 0 ||
	    size != V9958_FONT_SIZE) {
		return ZDRIVER_FAIL;
	}
	v9958_state.api = api;
	/* Cold bring-up: D0 uses the reset/default interrupt route.  All
	 * subsequent latch writes preserve this value in the shadow. */
	v9958_state.config_shadow = 0x00u;
	if (v9958_init_display() != ZDRIVER_OK) {
		return ZDRIVER_FAIL;
	}
	return ZDRIVER_OK;
}
