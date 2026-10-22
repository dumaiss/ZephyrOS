/* KonaOS V9958 video driver implementation (drivers/v9958/).
 *
 * The V9958 driver as a bank-6 driver image.  This is the
 * direct-hardware core ported from the pinned Zephyr-80 reference
 * (LunchCrema V9958, commit 2c6b9650): the exact GRAPHIC 6
 * initialization discipline, the native VIDEO operation surface, the
 * 6x8 CP850 font-atlas setup, the low-level graphics/command primitives,
 * and the output-only 85x26 text terminal renderer (ANSI/VT100-light
 * parser, circular R#23 scrolling, sprite cursor).  The port deliberately
 * drops the reference's CP/M dispatch table, BIOS addressing, HID entry
 * points, VIDEO_SEND payload ceiling and CP/M build layout.
 *
 * Driver layout (architecture 5.2):
 *   hot  this file and v9958_proto.c: the terminal renderer, the
 *        native operations, and the whole display bring-up chain - the
 *        KVIDEO_RESET path is reachable after boot and re-runs the same
 *        chain, so none of it is boot-only;
 *   boot v9958_boot.c: the boot-once marker and the direct font source
 *        of the initial upload (the boot-tier mapping shows the bulk
 *        data, the driver window does not);
 *   bulk the 2048-byte font itself, reached at run time through the
 *        nucleus bulk copy (api->bulk_read).
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
 * The font is the image's bulk segment (2048 bytes, packed from the
 * pinned asset): the initial upload reads it directly in the boot
 * tier's mode-10 mapping, and the run-time display reset re-reads it in
 * bounded 64-byte chunks through the nucleus bulk copy, because the
 * mode-11 driver window cannot show the boot/bulk tier.
 *
 * The bring-up leaves the VDP's interrupts off.  The frame interrupt is
 * an exclusive holder's to enable; the hold claims IRQ_VDP_FF and turns
 * it into a device event (v9958_proto.c).
 *
 * The buffers and state below live in the driver's DRIVER_IO data and
 * are relocated with the image.
 */

#include "kos/errno.h"
#include "driver.h"

#include "hardware.h"

#include "kos/protocol.h"

#include "v9958_driver.h"

struct v9958_state v9958_state;

/* The driver bootstrap table (set by the init entry) and the boot-tier
 * font source: the boot entry leaves the font's bank-6 address here for
 * the initial upload; the init clears it so every later (run-time) read
 * goes through the nucleus bulk copy. */
static const struct driver_api *v9958_api;
static uint16_t v9958_font_base;

/* Copy `length` font bytes at `offset` into `dst`. */
static int v9958_font_read(uint16_t offset, uint8_t *dst, uint16_t length)
{
	if (v9958_font_base != 0u) {
		/* Boot tier only: mode 10 maps the whole resident bank, so
		 * the bulk bytes are directly addressable. */
		kos_block_copy(dst, (const uint8_t *)
			       (uint16_t)(v9958_font_base + offset), length);
		return (int)length;
	}
	return (int)v9958_api->bulk_read(offset, (uint16_t)dst, length);
}

void v9958_set_font_base(uint16_t base)
{
	v9958_font_base = base;
}

void v9958_set_api(const struct driver_api *api)
{
	v9958_api = api;
}

/* Bounded scratch (module BSS, reused for every read/atlas segment):
 * one 8-glyph font chunk and one expanded 24-byte atlas segment.
 * The reverse flag mirrors the reference driver: the second pass swaps
 * foreground/background in the pair table. */
static uint8_t v9958_font_group[V9958_FONT_CHUNK];
static uint8_t v9958_atlas_row[V9958_ATLAS_SEG_BYTES];
static uint8_t v9958_cmd[KOS_V9958_CMD_REGISTER_COUNT];
static uint8_t v9958_atlas_reverse;

/* Print-run buffer (module BSS, relocated with the module image): the
 * reference batches up to 64 same-row characters and defers the cursor
 * SAT write and the present until a control operation, a flush point or
 * the right margin.  The base is addressed through a runtime pointer:
 * SDCC otherwise folds variable indexing into `add a,#<sym` /
 * `adc a,#>sym` byte-fragmented address arithmetic, which the MOD v1
 * relocator cannot express.  The pointer is established by the terminal
 * defaults (cold init and every reset); the assembly renderer reads the
 * run through it too. */
static uint8_t v9958_out_run[V9958_OUT_RUN_SIZE];
uint8_t *v9958_run_ptr;

/* Real-card G6 baseline from the verified bring-up sequence (R#0..R#11);
 * R#1 deliberately keeps the display disabled until VRAM setup ends. */
static const uint8_t v9958_g6_registers[12] = {
	KOS_V9958_R0_G6,             /* R#0: M5+M3 -> GRAPHIC 6          */
	KOS_V9958_R1_DISPLAY_OFF,    /* R#1: display off during setup   */
	KOS_V9958_R2_G6,             /* R#2: page-zero baseline         */
	0x00u,                       /* R#3                             */
	0x00u,                       /* R#4                             */
	KOS_V9958_R5_SPRITE_BASE,    /* R#5/R#11 -> color/SAT bases     */
	KOS_V9958_R6_SPRITE_PATTERN, /* R#6: sprite pattern table       */
	KOS_V9958_R7_BORDER,         /* R#7: text/backdrop color        */
	KOS_V9958_R8_64K_DRAM,       /* R#8: VR=1, 64Kx4 DRAMs          */
	KOS_V9958_R9_212_INTERLACE,  /* R#9: 212 lines, interlace       */
	0x00u,                       /* R#10                            */
	KOS_V9958_R11_SPRITE_ATTR    /* R#11                            */
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
				   KOS_V9958_CONFIG_INT_ROUTE) |
				  KOS_V9958_CONFIG_PORCH_OFF);

	v9958_state.config_shadow = value;
	v9958_io_latch_write(value);
	v9958_io_bootstrap_delay();
}

static void v9958_porch_on(void)
{
	uint8_t value = (uint8_t)(v9958_state.config_shadow &
				  KOS_V9958_CONFIG_INT_ROUTE);

	v9958_state.config_shadow = value;
	v9958_io_latch_write(value);
}

/* Write one precolored atlas row / cursor block at `addr` in VRAM's upper
 * 64 KiB (every atlas and cursor table lives there, A16 = 1).  R#14
 * carries A16..A14, then the two command-port address bytes select the
 * VRAM destination and the data stream follows.  A prior command always
 * completes first. */
static void v9958_write_vram(const uint8_t *src, uint16_t addr,
			     uint8_t count)
{
	uint8_t address[2];
	struct v9958_block block;

	v9958_io_wait_command();
	v9958_io_reg((uint8_t)(0x04u | ((uint8_t)(addr >> 14) & 0x03u)),
		     KOS_V9958_REG_R14);
	address[0] = (uint8_t)(addr & 0xFFu);
	address[1] = (uint8_t)(((uint8_t)(addr >> 8) & 0x3Fu) | 0x40u);
	block.src = address;
	block.length = 2u;
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
	v9958_io_reg(KOS_V9958_CMD_REGISTER_BASE, KOS_V9958_REG_R17);
	block.src = cmd;
	block.length = KOS_V9958_CMD_REGISTER_COUNT;
	v9958_io_stream_indirect(&block);
}

/* Zero the shared 15-byte command-engine block. */
static void v9958_cmd_reset(void)
{
	uint8_t *cmd = v9958_cmd;
	uint8_t n = KOS_V9958_CMD_REGISTER_COUNT;

	do {
		*cmd++ = 0x00u;
	} while (--n != 0u);
}

/* Physical page-zero scanline of logical text row `row`: one 8-line
 * cell per row on top of the circular R#23 origin, truncated to the
 * 256-line bitmap exactly like the reference. */
static uint8_t v9958_row_vram_y(uint8_t row)
{
	return (uint8_t)((uint8_t)(row * V9958_CELL_HEIGHT) +
			 v9958_state.scroll_origin);
}

/* Sprite cursor SAT: Y is the scanline above the cursor cell (the
 * hidden terminator when the cursor is off), X uses the sprite
 * coordinate space (three units per 6-pixel logical column). */
static void v9958_cursor_write_sat(void)
{
	uint8_t sat[V9958_CURSOR_SAT_BYTES];
	uint8_t x = (uint8_t)(v9958_state.text_col * 3u);
	uint8_t y;

	if (v9958_state.cursor_visible != 0u) {
		y = (uint8_t)(v9958_row_vram_y(v9958_state.text_row) - 1u);
	} else {
		y = V9958_CURSOR_TERMINATOR;
	}
	if (v9958_state.cursor_sat_valid != 0u &&
	    v9958_state.cursor_sat_visible == v9958_state.cursor_visible &&
	    (v9958_state.cursor_visible == 0u ||
	     (v9958_state.cursor_sat_x == x &&
	      v9958_state.cursor_sat_y == y))) {
		return;
	}
	sat[0] = y;
	sat[1] = x;
	sat[2] = 0x00u;
	sat[3] = 0x00u;
	sat[4] = V9958_CURSOR_TERMINATOR;
	sat[5] = 0x00u;
	sat[6] = 0x00u;
	sat[7] = 0x00u;
	v9958_write_vram(sat, V9958_CURSOR_SAT_ADDR, V9958_CURSOR_SAT_BYTES);
	v9958_state.cursor_sat_valid = 1u;
	v9958_state.cursor_sat_visible = v9958_state.cursor_visible;
	v9958_state.cursor_sat_x = x;
	v9958_state.cursor_sat_y = y;
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

	v9958_io_reg_paced(KOS_V9958_R25_WAIT_OFF, KOS_V9958_REG_R25);
	v9958_io_reg_paced(KOS_V9958_R8_64K_DRAM, KOS_V9958_REG_R8);

	/* A warm boot may follow a program that left a command active. */
	v9958_io_reg_paced(0x00u, KOS_V9958_REG_R46);

	for (i = 0u; i < 12u; ++i) {
		v9958_io_reg_paced(v9958_g6_registers[i], i);
	}

	/* Explicitly establish every selector/latch used by later
	 * helpers; R#26/R#27 are horizontal-scroll state that survives a
	 * warm boot. */
	v9958_io_reg_paced(0x00u, KOS_V9958_REG_R14);
	v9958_io_reg_paced(KOS_V9958_R15_STATUS2, KOS_V9958_REG_R15);
	v9958_io_reg_paced(0x00u, KOS_V9958_REG_R16);
	v9958_io_reg_paced(0x00u, KOS_V9958_REG_R17);
	v9958_io_reg_paced(0x00u, KOS_V9958_REG_R18);
	v9958_io_reg_paced(KOS_V9958_R23_TEXT_BASE, KOS_V9958_REG_R23);
	v9958_io_reg_paced(0x00u, KOS_V9958_REG_R26);
	v9958_io_reg_paced(0x00u, KOS_V9958_REG_R27);
}

/* Step 3: enable native WAIT first (WTE=1, VDS stays 0 so pin 8 keeps
 * supplying CPUCLK), then enable the U11 front porch. */
static void v9958_enable_hardware_wait(void)
{
	v9958_io_reg_paced(KOS_V9958_R25_WAIT_ON, KOS_V9958_REG_R25);
	v9958_porch_on();
}

/* Step 4: palette entries 0..15, each explicitly selected; every byte
 * is held by a real hardware WAIT, exactly as proven on the card.  The
 * software pacing is retained because it is harmless here. */
static void v9958_init_palette(void)
{
	uint8_t i;

	for (i = 0u; i < 16u; ++i) {
		v9958_io_reg_paced(i, KOS_V9958_REG_R16);
		v9958_write_palette_byte(v9958_palette[(uint16_t)i * 2u]);
		v9958_io_bootstrap_delay();
		v9958_write_palette_byte(
			v9958_palette[(uint16_t)i * 2u + 1u]);
		v9958_io_bootstrap_delay();
	}
}

/* Convert one 8-glyph chunk of the resident font into eight 24-byte
 * atlas segments and stream them to VRAM.  Four chunks form one
 * 32-glyph atlas row; the bytes written are identical to the previous
 * 32-glyph staging, only split into four smaller segments per scanline. */
static int v9958_upload_atlas_chunk(uint8_t chunk, uint8_t reverse)
{
	uint8_t scanline;
	uint8_t glyph;
	uint8_t row = (uint8_t)(chunk >> 2);      /* chunks per atlas row    */
	uint8_t segment = (uint8_t)(chunk & 3u);  /* segment within the row  */
	uint16_t addr;
	int rc;

	rc = v9958_font_read((uint16_t)((uint16_t)chunk * V9958_FONT_CHUNK),
			     v9958_font_group, V9958_FONT_CHUNK);
	if (rc != (int)V9958_FONT_CHUNK) {
		return V9958_FAIL;
	}

	if (reverse != 0u) {
		v9958_atlas_reverse = 1u;
	} else {
		v9958_atlas_reverse = 0u;
	}
	for (scanline = 0u; scanline < V9958_GLYPH_BYTES; ++scanline) {
		uint8_t s = (uint8_t)((uint8_t)(row << 3) | scanline);
		uint8_t *dst = &v9958_atlas_row[0];

		for (glyph = 0u; glyph < V9958_ATLAS_GLYPHS; ++glyph) {
			/* Address the chunk through a 16-bit base: an 8-bit
			 * `symbol + offset` makes the size optimizer emit
			 * byte-fragmented address relocations MOD v1
			 * cannot express. */
			uint8_t bits = *(const uint8_t *)(uint16_t)
				((uint16_t)v9958_font_group +
				 (uint16_t)((uint16_t)glyph *
					    V9958_GLYPH_BYTES) + scanline);

			v9958_expand_row(bits, dst);
			dst += 3u;
		}
		addr = (uint16_t)((uint16_t)s * V9958_ATLAS_PITCH +
				  (uint16_t)((uint16_t)segment *
					     V9958_ATLAS_SEG_BYTES));
		if (reverse != 0u) {
			addr = (uint16_t)(addr + V9958_ATLAS_REVERSE_ADDR);
			v9958_write_vram(v9958_atlas_row, addr,
					 V9958_ATLAS_SEG_BYTES);
		} else {
			addr = (uint16_t)(addr + V9958_ATLAS_NORMAL_ADDR);
			v9958_write_vram(v9958_atlas_row, addr,
					 V9958_ATLAS_SEG_BYTES);
		}
	}
	return V9958_OK;
}

/* Step 5a: the exact 2048-byte font is streamed through the resource
 * imports in bounded 64-byte chunks (one 8-glyph chunk per read).  The
 * size is validated up front (before any port I/O) so a missing or
 * wrong-sized resource cannot leave the VDP half-initialized. */
static int v9958_upload_font_atlas(void)
{
	uint8_t chunk;

	for (chunk = 0u; chunk < V9958_ATLAS_CHUNKS; ++chunk) {
		if (v9958_upload_atlas_chunk(chunk, 0u) != V9958_OK) {
			return V9958_FAIL;
		}
	}
	for (chunk = 0u; chunk < V9958_ATLAS_CHUNKS; ++chunk) {
		if (v9958_upload_atlas_chunk(chunk, 1u) != V9958_OK) {
			return V9958_FAIL;
		}
	}
	return V9958_OK;
}

/* Step 5b: clear all 256 lines of the circular page-zero bitmap and
 * restore its origin.  The fill uses the current SGR background so a
 * runtime clear (ESC [ 2 J, ESC c) repaints reverse video correctly; at
 * cold init / reset the attribute is the normal default (44h). */
static void v9958_clear_screen(void)
{
	uint8_t i;

	v9958_state.scroll_origin = 0u;
	v9958_io_reg(KOS_V9958_R23_TEXT_BASE, KOS_V9958_REG_R23);
	for (i = 0u; i < KOS_V9958_CMD_REGISTER_COUNT; ++i) {
		v9958_cmd[i] = 0x00u;
	}
	v9958_cmd[V9958_CMD_NX_LO] = V9958_CLEAR_WIDTH_LO;
	v9958_cmd[V9958_CMD_NX_HI] = V9958_CLEAR_WIDTH_HI;
	v9958_cmd[V9958_CMD_NY_LO] = V9958_CLEAR_HEIGHT_LO;
	v9958_cmd[V9958_CMD_NY_HI] = V9958_CLEAR_HEIGHT_HI;
	v9958_cmd[V9958_CMD_COLOR] =
		(v9958_state.current_attr & 0x01u) != 0u ?
		V9958_FILL_COLOR_REVERSE : V9958_CLEAR_COLOR;
	v9958_cmd[V9958_CMD_CODE] = V9958_CMD_HMMV;
	v9958_start_command(v9958_cmd);
	v9958_io_wait_command();
}

/* Step 5c: sprite cursor -- pattern, color table and SAT.  The SAT is
 * derived from the terminal cursor state (col/row/visibility), so the
 * cold bring-up and the runtime cursor updates share one writer. */
static void v9958_cursor_init(void)
{
	v9958_write_vram(v9958_cursor_pattern, V9958_CURSOR_PATTERN_ADDR, 8u);
	v9958_write_vram(v9958_cursor_colors, V9958_CURSOR_COLOR_ADDR,
			 V9958_CURSOR_COLORS);
	/* A display reset rewrites VRAM, so the software cache cannot prove
	 * that the SAT still contains its last remembered value. */
	v9958_state.cursor_sat_valid = 0u;
	v9958_cursor_write_sat();
}

/* The complete bring-up in the proven order.  Re-establishing this on
 * the KVIDEO_RESET operation is exactly the reference reset path: every
 * owned register, selector, palette entry and VRAM asset is rewritten,
 * the latch shadow (and therefore D0) is preserved, and the display is
 * left enabled. */
static int v9958_init_display(void)
{
	v9958_init_g6();
	v9958_enable_hardware_wait();
	v9958_init_palette();
	if (v9958_upload_font_atlas() != V9958_OK) {
		return V9958_FAIL;
	}
	v9958_clear_screen();
	v9958_cursor_init();
	v9958_io_wait_command(); /* present */
	v9958_io_reg(KOS_V9958_R1_DISPLAY_ON, KOS_V9958_REG_R1);
	return V9958_OK;
}

/* ======================================================================
 * Output-only 85x26 text terminal (ported reference renderer)
 * ======================================================================
 *
 * The byte stream arrives exclusively through the terminal entries.  The
 * drawing algorithms use HMMM copies from the precolored atlas page selected
 * by the current SGR attribute, merging atlas-contiguous glyphs into one
 * wider copy and consecutive spaces into one HMMV background fill,
 * erases are HMMV fills in the current background, scrolling advances the
 * circular R#23 origin by one 8-line cell and repaints only the fixed
 * margin and the newly exposed bottom row, and the sprite SAT tracks the
 * logical cursor.  Print runs batch up to 64 glyphs; every control
 * operation, cursor update and blocking point flushes them first, so
 * ordinary strings stay efficient and are never lost behind a later
 * operation.  Unknown or partial sequences are consumed and recovered
 * exactly like the reference; every coordinate is clamped, and every
 * fill is split at the end of the circular bitmap, so no parser input can
 * write outside the terminal state or the page-zero bitmap.
 */

/* HMMV fill color for the current SGR attribute. */
static uint8_t v9958_fill_color(void)
{
	return (v9958_state.current_attr & 0x01u) != 0u ?
		V9958_FILL_COLOR_REVERSE : V9958_CLEAR_COLOR;
}

/* Defined below; used by the parser's reset paths. */
static void v9958_term_set_defaults(void);
static void v9958_put_printable(uint8_t ch);

/* Rectangle erase in cells: width x height cells at (col,row), filled
 * with the current background.  A rectangle crossing the end of the
 * circular page-zero bitmap is split at physical line 255 (the reference
 * split) so the command engine can never run into the atlas page. */
static void v9958_fill_cells(uint8_t col, uint8_t row, uint8_t width,
			     uint8_t height)
{
	uint16_t dx = (uint16_t)col * V9958_CELL_WIDTH;
	uint16_t nx = (uint16_t)width * V9958_CELL_WIDTH;
	uint8_t ny = (uint8_t)(height * V9958_CELL_HEIGHT);
	uint8_t dy = v9958_row_vram_y(row);

	v9958_cmd_reset();
	v9958_cmd[V9958_CMD_DX_LO] = (uint8_t)(dx & 0xFFu);
	v9958_cmd[V9958_CMD_DX_HI] = (uint8_t)(dx >> 8);
	v9958_cmd[V9958_CMD_DY_LO] = dy;
	v9958_cmd[V9958_CMD_NX_LO] = (uint8_t)(nx & 0xFFu);
	v9958_cmd[V9958_CMD_NX_HI] = (uint8_t)(nx >> 8);
	v9958_cmd[V9958_CMD_NY_LO] = ny;
	v9958_cmd[V9958_CMD_COLOR] = v9958_fill_color();
	v9958_cmd[V9958_CMD_CODE] = V9958_CMD_HMMV;

	if ((uint8_t)(dy + ny) < dy) {
		uint8_t wrapped = (uint8_t)(dy + ny);

		v9958_cmd[V9958_CMD_NY_LO] = (uint8_t)(0u - dy);
		v9958_start_command(v9958_cmd);
		if (wrapped == 0u) {
			return;
		}
		v9958_cmd[V9958_CMD_DY_LO] = 0x00u;
		v9958_cmd[V9958_CMD_NY_LO] = wrapped;
	}
	v9958_start_command(v9958_cmd);
}

/* Copy `count` full text rows; `bottom_up` selects the reference's
 * bottom-to-top order for overlapping insert-line moves. */
static void v9958_copy_rows(uint8_t src_row, uint8_t dst_row,
			    uint8_t count, uint8_t bottom_up)
{
	if (count == 0u) {
		return;
	}
	if (bottom_up != 0u) {
		src_row = (uint8_t)(src_row + count - 1u);
		dst_row = (uint8_t)(dst_row + count - 1u);
	}
	while (count != 0u) {
		v9958_cmd_reset();
		v9958_cmd[V9958_CMD_SY_LO] = v9958_row_vram_y(src_row);
		v9958_cmd[V9958_CMD_DY_LO] = v9958_row_vram_y(dst_row);
		v9958_cmd[V9958_CMD_NX_LO] =
			(uint8_t)(V9958_PIXEL_WIDTH & 0xFFu);
		v9958_cmd[V9958_CMD_NX_HI] =
			(uint8_t)(V9958_PIXEL_WIDTH >> 8);
		v9958_cmd[V9958_CMD_NY_LO] = V9958_CELL_HEIGHT;
		v9958_cmd[V9958_CMD_CODE] = V9958_CMD_HMMM;
		v9958_start_command(v9958_cmd);
		if (bottom_up != 0u) {
			--src_row;
			--dst_row;
		} else {
			++src_row;
			++dst_row;
		}
		--count;
	}
}

/* --- print run ------------------------------------------------------- */

/* The pending run is rendered by v9958_out_flush (v9958_io.asm): one
 * HMMM glyph copy per character from the precolored atlas, the run only
 * spanning one text row (appending at the right margin flushes first,
 * see v9958_put_printable). */

/* Make everything written so far visible: flush the run, write the
 * cursor SAT and wait for the command engine (present).  This is the
 * reference's cursor_set_position_current and the flush point used by
 * every operation that moves the cursor or blocks. */
static void v9958_term_sync(void)
{
	v9958_out_flush();
	v9958_cursor_write_sat();
	v9958_io_wait_command();
}

/* --- absolute render transactions ----------------------------------- */

/* LMMV is a dot operation in GRAPHIC 6.  CLR is therefore the four-bit
 * blue/white colour XOR, not the packed byte used by HMMV fills. */
void v9958_render_text_data(const uint8_t *buf, uint16_t len)
{
	while (len-- != 0u) {
		v9958_put_printable(*buf++);
	}
}

void v9958_render_invert(uint8_t x, uint8_t y, uint8_t count)
{
	uint16_t dx = (uint16_t)x * V9958_CELL_WIDTH;
	uint16_t nx = (uint16_t)count * V9958_CELL_WIDTH;
	uint8_t dy = v9958_row_vram_y(y);
	uint8_t ny = V9958_CELL_HEIGHT;

	v9958_out_flush();
	v9958_cmd_reset();
	v9958_cmd[V9958_CMD_DX_LO] = (uint8_t)(dx & 0xFFu);
	v9958_cmd[V9958_CMD_DX_HI] = (uint8_t)(dx >> 8);
	v9958_cmd[V9958_CMD_DY_LO] = dy;
	v9958_cmd[V9958_CMD_NX_LO] = (uint8_t)(nx & 0xFFu);
	v9958_cmd[V9958_CMD_NX_HI] = (uint8_t)(nx >> 8);
	v9958_cmd[V9958_CMD_NY_LO] = ny;
	v9958_cmd[V9958_CMD_COLOR] = V9958_INVERT_COLOR;
	v9958_cmd[V9958_CMD_CODE] = V9958_CMD_LMMV_EOR;

	if ((uint8_t)(dy + ny) < dy) {
		uint8_t wrapped = (uint8_t)(dy + ny);

		v9958_cmd[V9958_CMD_NY_LO] = (uint8_t)(0u - dy);
		v9958_start_command(v9958_cmd);
		if (wrapped == 0u) {
			return;
		}
		v9958_cmd[V9958_CMD_DY_LO] = 0u;
		v9958_cmd[V9958_CMD_NY_LO] = wrapped;
	}
	v9958_start_command(v9958_cmd);
}

void v9958_render_finish_worker(void)
{
	v9958_term_sync();
	v9958_state.render_active = 0u;
}

/* --- terminal cursor and text actions -------------------------------- */

/* Scroll the circular bitmap by one cell: advance the R#23 origin,
 * repaint the fixed margin before logical row zero and the newly
 * exposed bottom row, then commit the new origin.  The reference
 * deliberately trades one tearing field for line-rate scrolling instead
 * of waiting for vertical retrace on every line. */
static void v9958_scroll_up(void)
{
	uint8_t origin =
		(uint8_t)(v9958_state.scroll_origin + V9958_CELL_HEIGHT);

	v9958_state.scroll_origin = origin;

	v9958_cmd_reset();
	v9958_cmd[V9958_CMD_DY_LO] =
		(uint8_t)(origin - V9958_DISPLAY_OFFSET);
	v9958_cmd[V9958_CMD_NX_LO] = V9958_CLEAR_WIDTH_LO;
	v9958_cmd[V9958_CMD_NX_HI] = V9958_CLEAR_WIDTH_HI;
	v9958_cmd[V9958_CMD_NY_LO] = V9958_DISPLAY_OFFSET;
	v9958_cmd[V9958_CMD_COLOR] = V9958_CLEAR_COLOR;
	v9958_cmd[V9958_CMD_CODE] = V9958_CMD_HMMV;
	v9958_start_command(v9958_cmd);

	v9958_fill_cells(0u, V9958_SCROLL_BOTTOM, V9958_COLUMNS, 1u);
	v9958_io_wait_command();

	v9958_io_reg((uint8_t)(origin - V9958_DISPLAY_OFFSET),
		     KOS_V9958_REG_R23);
}

/* Terminal-level scroll: flush, scroll the bitmap, park the cursor at
 * the bottom row and present (reference text_scroll_up). */
static void v9958_text_scroll_up(void)
{
	v9958_out_flush();
	v9958_scroll_up();
	v9958_state.text_col = 0u;
	v9958_state.text_row = V9958_SCROLL_BOTTOM;
	v9958_term_sync();
}

/* Move the cursor one cell right, wrapping (or clamping and
 * overwriting) at the right margin exactly per the reference. */
static void v9958_advance_cursor(void)
{
	if ((uint8_t)(v9958_state.text_col + 1u) < V9958_COLUMNS) {
		v9958_state.text_col =
			(uint8_t)(v9958_state.text_col + 1u);
		return;
	}
	if (v9958_state.auto_wrap != 0u) {
		v9958_state.text_col = 0u;
		if ((uint8_t)(v9958_state.text_row + 1u) < V9958_ROWS) {
			v9958_state.text_row =
				(uint8_t)(v9958_state.text_row + 1u);
		} else {
			v9958_text_scroll_up();
		}
	} else {
		v9958_state.text_col = V9958_COLUMNS - 1u;
	}
}

/* Reference text_newline: flush, column zero, next row or scroll. */
static void v9958_newline(void)
{
	v9958_out_flush();
	v9958_state.text_col = 0u;
	if ((uint8_t)(v9958_state.text_row + 1u) < V9958_ROWS) {
		v9958_state.text_row =
			(uint8_t)(v9958_state.text_row + 1u);
	} else {
		v9958_text_scroll_up();
	}
}

static void v9958_put_printable(uint8_t ch)
{
	uint8_t *run = v9958_run_ptr;

	if (v9958_state.out_run_count == V9958_OUT_RUN_SIZE) {
		if (v9958_state.render_active != 0u) {
			v9958_out_flush();
		} else {
			v9958_term_sync();
		}
	}
	if (v9958_state.out_run_count == 0u) {
		v9958_state.out_run_col = v9958_state.text_col;
		v9958_state.out_run_row = v9958_state.text_row;
	}
	run[v9958_state.out_run_count] = ch;
	v9958_state.out_run_count =
		(uint8_t)(v9958_state.out_run_count + 1u);

	if (v9958_state.text_col == (V9958_COLUMNS - 1u)) {
		v9958_out_flush();
		if (v9958_state.render_active == 0u) {
			v9958_advance_cursor();
			v9958_term_sync();
		}
	} else {
		v9958_advance_cursor();
	}
}

static void v9958_term_cr(void)
{
	v9958_state.text_col = 0u;
	v9958_term_sync();
}

static void v9958_term_lf(void)
{
	uint8_t col = v9958_state.text_col;

	v9958_newline();
	v9958_state.text_col = col;
	v9958_term_sync();
}

static void v9958_term_nel(void)
{
	v9958_state.text_col = 0u;
	v9958_newline();
	v9958_term_sync();
}

/* Reverse index: one row up, no scroll-down at the top (deferred in the
 * reference and deliberately still deferred here). */
static void v9958_term_ri(void)
{
	if (v9958_state.text_row == 0u) {
		return;
	}
	v9958_state.text_row = (uint8_t)(v9958_state.text_row - 1u);
	v9958_term_sync();
}

static void v9958_term_bs(void)
{
	if (v9958_state.text_col == 0u) {
		return;
	}
	v9958_state.text_col = (uint8_t)(v9958_state.text_col - 1u);
	v9958_term_sync();
}

/* Advance to the next 8-column tab stop (always at least one cell). */
static void v9958_term_tab(void)
{
	do {
		v9958_advance_cursor();
	} while ((v9958_state.text_col & 0x07u) != 0u);
	v9958_term_sync();
}

static void v9958_term_cursor_up(void)
{
	if (v9958_state.text_row == 0u) {
		return;
	}
	v9958_state.text_row = (uint8_t)(v9958_state.text_row - 1u);
	v9958_term_sync();
}

static void v9958_term_cursor_down(void)
{
	if (v9958_state.text_row >= (V9958_ROWS - 1u)) {
		return;
	}
	v9958_state.text_row = (uint8_t)(v9958_state.text_row + 1u);
	v9958_term_sync();
}

static void v9958_term_cursor_left(void)
{
	if (v9958_state.text_col == 0u) {
		return;
	}
	v9958_state.text_col = (uint8_t)(v9958_state.text_col - 1u);
	v9958_term_sync();
}

static void v9958_term_cursor_right(void)
{
	if (v9958_state.text_col >= (V9958_COLUMNS - 1u)) {
		return;
	}
	v9958_state.text_col = (uint8_t)(v9958_state.text_col + 1u);
	v9958_term_sync();
}

/* --- erasing ---------------------------------------------------------- */

static void v9958_clear_to_eol(void)
{
	v9958_out_flush();
	v9958_fill_cells(v9958_state.text_col, v9958_state.text_row,
			 (uint8_t)(V9958_COLUMNS - v9958_state.text_col), 1u);
}

static void v9958_clear_line(void)
{
	uint8_t col = v9958_state.text_col;

	v9958_state.text_col = 0u;
	v9958_clear_to_eol();
	v9958_state.text_col = col;
}

static void v9958_clear_from_sol_to_cursor(void)
{
	v9958_out_flush();
	v9958_fill_cells(0u, v9958_state.text_row,
			 (uint8_t)(v9958_state.text_col + 1u), 1u);
}

/* ED 0: cursor through end of screen; the cursor is restored. */
static void v9958_ed_to_eos(void)
{
	uint8_t col = v9958_state.text_col;
	uint8_t row = v9958_state.text_row;
	uint8_t scan = (uint8_t)(row + 1u);

	v9958_clear_to_eol();
	while (scan < V9958_ROWS) {
		v9958_state.text_row = scan;
		v9958_state.text_col = 0u;
		v9958_clear_to_eol();
		++scan;
	}
	v9958_state.text_col = col;
	v9958_state.text_row = row;
	v9958_term_sync();
}

/* ED 1: screen start through cursor; the cursor is restored. */
static void v9958_ed_from_start(void)
{
	uint8_t col = v9958_state.text_col;
	uint8_t row = v9958_state.text_row;
	uint8_t scan;

	for (scan = 0u; scan < row; ++scan) {
		v9958_state.text_row = scan;
		v9958_state.text_col = 0u;
		v9958_clear_line();
	}
	v9958_state.text_row = row;
	v9958_state.text_col = col;
	v9958_clear_from_sol_to_cursor();
	v9958_term_sync();
}

/* ED 2 / FF: clear the whole screen and home (reference compatibility
 * behavior). */
static void v9958_clear_screen_runtime(void)
{
	v9958_out_flush();
	v9958_clear_screen();
	v9958_state.text_col = 0u;
	v9958_state.text_row = 0u;
	v9958_term_sync();
}

/* --- insert/delete lines (full-screen region, reference clamp) -------- */

static void v9958_insert_lines(uint8_t n)
{
	uint8_t row = v9958_state.text_row;
	uint8_t shift = (uint8_t)(V9958_ROWS - row - n);

	if (shift != 0u) {
		v9958_copy_rows(row, (uint8_t)(row + n), shift, 1u);
	}
	v9958_fill_cells(0u, row, V9958_COLUMNS, n);
}

static void v9958_delete_lines(uint8_t n)
{
	uint8_t row = v9958_state.text_row;
	uint8_t shift = (uint8_t)(V9958_ROWS - row - n);

	if (shift != 0u) {
		v9958_copy_rows((uint8_t)(row + n), row, shift, 0u);
	}
	v9958_fill_cells(0u, (uint8_t)(V9958_ROWS - n), V9958_COLUMNS, n);
}

/* --- ANSI/CSI operations ---------------------------------------------- */

static uint8_t v9958_param0_default1(void)
{
	if (v9958_state.csi_param_count == 0u ||
	    v9958_state.csi_param0 == 0u) {
		return 1u;
	}
	return v9958_state.csi_param0;
}

static uint8_t v9958_param1_default1(void)
{
	if (v9958_state.csi_param_count < 2u ||
	    v9958_state.csi_param1 == 0u) {
		return 1u;
	}
	return v9958_state.csi_param1;
}

static void v9958_store_param(void)
{
	if (v9958_state.csi_param_count >= 2u) {
		v9958_state.csi_accum = 0u; /* extra params are dropped */
		return;
	}
	if (v9958_state.csi_param_count == 0u) {
		v9958_state.csi_param0 = v9958_state.csi_accum;
	} else {
		v9958_state.csi_param1 = v9958_state.csi_accum;
	}
	v9958_state.csi_param_count =
		(uint8_t)(v9958_state.csi_param_count + 1u);
	v9958_state.csi_accum = 0u;
}

static void v9958_ansi_cursor_up_n(uint8_t n)
{
	while (n != 0u) {
		v9958_term_cursor_up();
		--n;
	}
}

static void v9958_ansi_cursor_down_n(uint8_t n)
{
	while (n != 0u) {
		v9958_term_cursor_down();
		--n;
	}
}

static void v9958_ansi_cursor_left_n(uint8_t n)
{
	while (n != 0u) {
		v9958_term_cursor_left();
		--n;
	}
}

static void v9958_ansi_cursor_right_n(uint8_t n)
{
	while (n != 0u) {
		v9958_term_cursor_right();
		--n;
	}
}

/* CSI G: absolute column (1-based, clamped). */
static void v9958_ansi_cha(void)
{
	uint8_t col = (uint8_t)(v9958_param0_default1() - 1u);

	if (col >= V9958_COLUMNS) {
		col = V9958_COLUMNS - 1u;
	}
	v9958_state.text_col = col;
	v9958_term_sync();
}

/* CSI d: absolute row (1-based, clamped). */
static void v9958_ansi_vpa(void)
{
	uint8_t row = (uint8_t)(v9958_param0_default1() - 1u);

	if (row >= V9958_ROWS) {
		row = V9958_ROWS - 1u;
	}
	v9958_state.text_row = row;
	v9958_term_sync();
}

/* CSI H / CSI f: cursor position (row;col, 1-based, clamped). */
static void v9958_ansi_cup(void)
{
	uint8_t row = (uint8_t)(v9958_param0_default1() - 1u);
	uint8_t col;

	if (row >= V9958_ROWS) {
		row = V9958_ROWS - 1u;
	}
	v9958_state.text_row = row;

	col = (uint8_t)(v9958_param1_default1() - 1u);
	if (col >= V9958_COLUMNS) {
		col = V9958_COLUMNS - 1u;
	}
	v9958_state.text_col = col;
	v9958_term_sync();
}

/* CSI J: erase in display. */
static void v9958_ansi_ed(void)
{
	if (v9958_state.csi_param_count == 0u ||
	    v9958_state.csi_param0 == 0u) {
		v9958_ed_to_eos();
		return;
	}
	if (v9958_state.csi_param0 == 1u) {
		v9958_ed_from_start();
		return;
	}
	if (v9958_state.csi_param0 == 2u) {
		v9958_clear_screen_runtime();
	}
}

/* CSI K: erase in line. */
static void v9958_ansi_el(void)
{
	if (v9958_state.csi_param_count == 0u ||
	    v9958_state.csi_param0 == 0u) {
		v9958_clear_to_eol();
		return;
	}
	if (v9958_state.csi_param0 == 1u) {
		v9958_clear_from_sol_to_cursor();
		return;
	}
	v9958_clear_line();
}

/* CSI X: erase n characters from the cursor (clamped to the line). */
static void v9958_ansi_ech(void)
{
	uint8_t n = v9958_param0_default1();
	uint8_t avail =
		(uint8_t)(V9958_COLUMNS - v9958_state.text_col);

	if (avail == 0u) {
		return;
	}
	if (n > avail) {
		n = avail;
	}
	v9958_out_flush();
	v9958_fill_cells(v9958_state.text_col, v9958_state.text_row, n, 1u);
}

/* SGR parameter effect: 0 reset, 7 reverse on, 27 reverse off; every
 * other parameter (bold/underline/blink/colors) is consumed without a
 * visual effect, as in the reference. */
static void v9958_sgr_apply(uint8_t param)
{
	if (param == 0u) {
		v9958_state.current_attr = 0u;
	} else if (param == 7u) {
		v9958_state.current_attr = 1u;
	} else if (param == 27u) {
		v9958_state.current_attr = 0u;
	}
}

static void v9958_ansi_sgr(void)
{
	v9958_out_flush();
	if (v9958_state.csi_param_count == 0u) {
		v9958_state.current_attr = 0u;
		return;
	}
	v9958_sgr_apply(v9958_state.csi_param0);
	if (v9958_state.csi_param_count >= 2u) {
		v9958_sgr_apply(v9958_state.csi_param1);
	}
}

static void v9958_ansi_save_cursor(void)
{
	v9958_state.saved_col = v9958_state.text_col;
	v9958_state.saved_row = v9958_state.text_row;
	v9958_state.saved_attr = v9958_state.current_attr;
}

static void v9958_ansi_restore_cursor(void)
{
	v9958_state.text_col = v9958_state.saved_col;
	v9958_state.text_row = v9958_state.saved_row;
	v9958_state.current_attr = v9958_state.saved_attr;
	v9958_term_sync();
}

/* DEC private set (ESC [ ? n h): 7 = DECAWM on, 25 = show cursor;
 * others are consumed. */
static void v9958_ansi_decset(void)
{
	if (v9958_state.csi_param_count == 0u) {
		return;
	}
	if (v9958_state.csi_param0 == 7u) {
		v9958_state.auto_wrap = 1u;
		return;
	}
	if (v9958_state.csi_param0 == 25u) {
		v9958_out_flush();
		v9958_state.cursor_visible = 1u;
		v9958_cursor_write_sat();
	}
}

/* DEC private reset (ESC [ ? n l): 7 = DECAWM off, 25 = hide cursor;
 * others are consumed. */
static void v9958_ansi_decrst(void)
{
	if (v9958_state.csi_param_count == 0u) {
		return;
	}
	if (v9958_state.csi_param0 == 7u) {
		v9958_state.auto_wrap = 0u;
		return;
	}
	if (v9958_state.csi_param0 == 25u) {
		v9958_out_flush();
		v9958_state.cursor_visible = 0u;
		v9958_cursor_write_sat();
	}
}

/* CSI L / CSI M: insert/delete n lines at the cursor row (full-screen
 * region; n clamped to the rows below the cursor). */
static void v9958_ansi_insert_lines(void)
{
	uint8_t n = v9958_param0_default1();
	uint8_t avail = (uint8_t)(V9958_ROWS - v9958_state.text_row);

	if (n > avail) {
		n = avail;
	}
	v9958_out_flush();
	v9958_insert_lines(n);
}

static void v9958_ansi_delete_lines(void)
{
	uint8_t n = v9958_param0_default1();
	uint8_t avail = (uint8_t)(V9958_ROWS - v9958_state.text_row);

	if (n > avail) {
		n = avail;
	}
	v9958_out_flush();
	v9958_delete_lines(n);
}

/* Dispatch one CSI final byte (parameters already stored). */
static void v9958_dispatch_csi(uint8_t command)
{
	if (v9958_state.csi_private != 0u) {
		if (command == 'h') {
			v9958_ansi_decset();
		} else if (command == 'l') {
			v9958_ansi_decrst();
		}
		return; /* unsupported DEC private: consumed */
	}

	switch (command) {
	case 'G':
		v9958_ansi_cha();
		break;
	case 'A':
		v9958_ansi_cursor_up_n(v9958_param0_default1());
		break;
	case 'B':
		v9958_ansi_cursor_down_n(v9958_param0_default1());
		break;
	case 'C':
		v9958_ansi_cursor_right_n(v9958_param0_default1());
		break;
	case 'D':
		v9958_ansi_cursor_left_n(v9958_param0_default1());
		break;
	case 'H':
	case 'f':
		v9958_ansi_cup();
		break;
	case 'd':
		v9958_ansi_vpa();
		break;
	case 'J':
		v9958_ansi_ed();
		break;
	case 'K':
		v9958_ansi_el();
		break;
	case 'X':
		v9958_ansi_ech();
		break;
	case 'm':
		v9958_ansi_sgr();
		break;
	case 's':
		v9958_ansi_save_cursor();
		break;
	case 'u':
		v9958_ansi_restore_cursor();
		break;
	case 'L':
		v9958_ansi_insert_lines();
		break;
	case 'M':
		v9958_ansi_delete_lines();
		break;
	default:
		/* 'c', 'n' (device attributes/status) and every unknown
		 * final byte: consumed safely. */
		break;
	}
}

/* --- parser ----------------------------------------------------------- */

static void v9958_term_csi(uint8_t byte)
{
	if (byte >= '0' && byte <= '9') {
		v9958_state.csi_accum = (uint8_t)(
			(uint8_t)(v9958_state.csi_accum * 10u) +
			(uint8_t)(byte - '0'));
		return;
	}
	if (byte == '?') {
		if (v9958_state.csi_param_count == 0u) {
			v9958_state.csi_private = 1u;
		}
		return;
	}
	if (byte == ';') {
		v9958_store_param();
		return;
	}
	v9958_store_param();
	v9958_state.term_state = V9958_TERM_NORMAL;
	v9958_state.esc_count = 0u;
	v9958_dispatch_csi(byte);
}

static void v9958_term_esc(uint8_t byte)
{
	v9958_state.term_state = V9958_TERM_NORMAL;

	if (byte == 0x1Bu) {
		v9958_out_flush();
		if (v9958_state.esc_count == 2u) {
			/* Third ESC: full display reset (RIS). */
			v9958_term_set_defaults();
			(void)v9958_init_display();
			return;
		}
		v9958_state.esc_count =
			(uint8_t)(v9958_state.esc_count + 1u);
		v9958_state.term_state = V9958_TERM_ESC;
		return;
	}
	v9958_state.esc_count = 0u;

	switch (byte) {
	case '[':
		v9958_state.term_state = V9958_TERM_CSI;
		v9958_state.csi_param_count = 0u;
		v9958_state.csi_accum = 0u;
		v9958_state.csi_private = 0u;
		v9958_state.csi_param0 = 0u;
		v9958_state.csi_param1 = 0u;
		break;
	case '#':
		v9958_state.term_state = V9958_TERM_ESC_HASH;
		break;
	case '(':
	case ')':
	case 'O':
		v9958_state.term_state = V9958_TERM_CHARSET;
		break;
	case 'D':
		v9958_term_lf(); /* IND */
		break;
	case 'E':
		v9958_term_nel();
		break;
	case 'M':
		v9958_term_ri(); /* RI; scroll-down at top deferred */
		break;
	case '7':
		v9958_ansi_save_cursor();
		break;
	case '8':
		v9958_ansi_restore_cursor();
		break;
	case 'c':
		v9958_term_set_defaults();
		(void)v9958_init_display();
		break;
	default:
		/* ESC H/Z/=/ > and unknown ESC finals: consumed. */
		break;
	}
}

/* One byte through the ESC / CSI / charset state machine.  The write
 * path's plain printable bytes take the assembly fast path
 * (v9958_term_write), which calls this for every other byte. */
void v9958_term_feed(uint8_t byte)
{
	if (v9958_state.term_state == V9958_TERM_ESC) {
		v9958_term_esc(byte);
		return;
	}
	if (v9958_state.term_state == V9958_TERM_CSI) {
		v9958_term_csi(byte);
		return;
	}
	if (v9958_state.term_state == V9958_TERM_ESC_HASH ||
	    v9958_state.term_state == V9958_TERM_CHARSET) {
		/* ESC # x / ESC ( ) O x: consume the one final byte. */
		v9958_state.term_state = V9958_TERM_NORMAL;
		v9958_state.esc_count = 0u;
		return;
	}

	if (byte == 0x1Bu) {
		v9958_out_flush();
		if (v9958_state.esc_count == 2u) {
			/* Third ESC: full display reset (RIS). */
			v9958_term_set_defaults();
			(void)v9958_init_display();
			return;
		}
		v9958_state.esc_count =
			(uint8_t)(v9958_state.esc_count + 1u);
		v9958_state.term_state = V9958_TERM_ESC;
		return;
	}
	v9958_state.esc_count = 0u;

	if (byte == 0x08u) {
		v9958_term_bs();
		return;
	}
	if (byte == 0x09u) {
		v9958_term_tab();
		return;
	}
	if (byte == 0x0Du) {
		v9958_term_cr();
		return;
	}
	if (byte == 0x0Au || byte == 0x0Bu) {
		v9958_term_lf(); /* LF and VT */
		return;
	}
	if (byte == 0x0Cu) {
		v9958_clear_screen_runtime(); /* FF */
		return;
	}
	if (byte >= 0x20u) {
		v9958_put_printable(byte);
		return;
	}
	/* NUL/BEL/SO/SI and every other control byte: consume after
	 * flushing the pending run (reference behavior). */
	v9958_out_flush();
}

/* Parser/cursor/rendition defaults shared by cold init and every reset
 * (KVIDEO_RESET, ESC c, triple-ESC): normal video, auto-wrap on, cursor
 * visible at the home cell, no saved state. */
static void v9958_term_set_defaults(void)
{
	/* term_state through out_run_count are contiguous (the offsets
	 * v9958_driver.h pins): zero them, then the two that default on. */
	uint8_t *field = &v9958_state.term_state;
	uint8_t i;

	for (i = 0u; i < (uint8_t)(V9958_ST_DEFAULTS_END - 2u); ++i) {
		field[i] = 0u; /* V9958_TERM_NORMAL, home, normal video */
	}
	v9958_state.cursor_visible = 1u;
	v9958_state.auto_wrap = 1u;
	v9958_run_ptr = v9958_out_run;
}

/* --- generic character interface (output only) ------------------------
 *
 * The terminal entries are assembly wrappers (v9958_io.asm) that run
 * these C workers on a private module renderer stack: the kernel syscall
 * gate stack is only about 70 bytes deep, which the ported renderer's
 * call chain exceeds.  The wrappers preserve the exact sdcccall(1)
 * prototypes and return values; the workers must stay single-depth
 * (they are never re-entered).
 */

/* 1 while a program holds the object exclusively.  The console must not
 * draw over the screen the holder owns, so its terminal bytes are dropped
 * and counted; the count is the takeover's evidence, not a protocol
 * answer.  The RENDER worker (v9958_proto.c) counts its stream length
 * through the same check. */
int v9958_overtaken(struct v9958_state *state, uint16_t amount)
{
	if (state->exclusive == 0u) {
		return 0u;
	}
	if (state->dropped > (uint16_t)(0xFFFFu - amount)) {
		state->dropped = 0xFFFFu;
	} else {
		state->dropped = (uint16_t)(state->dropped + amount);
	}
	return 1u;
}

int v9958_char_putc_worker(void *vstate, char ch)
{
	struct v9958_state *state = (struct v9958_state *)vstate;

	if (state == (struct v9958_state *)0) {
		return (-KOS_ENODEV);
	}
	if (v9958_overtaken(state, 1u) != 0u) {
		return 0;
	}
	/* A standalone byte has no later flush opportunity in the console
	 * driver, so putc completes with a visible update. */
	v9958_term_feed((uint8_t)ch);
	v9958_term_sync();
	return 0;
}

int v9958_char_write_worker(void *vstate, const char *buf, uint16_t len)
{
	struct v9958_state *state = (struct v9958_state *)vstate;

	if (state == (struct v9958_state *)0) {
		return (-KOS_ENODEV);
	}
	if (v9958_overtaken(state, len) != 0u) {
		return (int)len; /* dropped: the caller keeps its count */
	}
	if (len == 0u) {
		return 0;
	}
	if (buf == (const char *)0) {
		return (-KOS_EINVAL);
	}
	/* The whole 16-bit transfer is processed; the print run batches up
	 * to 64 glyphs internally and one final sync makes it visible. */
	v9958_term_write((const uint8_t *)buf, len);
	v9958_term_sync();
	return (int)len;
}

/* --- class operations --------------------------------------------------- */

int v9958_send(void *vstate, uint8_t operation, const void *payload,
	       uint16_t length)
{
	struct v9958_state *state = (struct v9958_state *)vstate;
	struct v9958_block block;

	if (state == (struct v9958_state *)0) {
		return (-KOS_ENODEV);
	}

	switch (operation) {
	case KVIDEO_CTRL_WRITE:
	case KVIDEO_DATA_WRITE:
	case KVIDEO_DATA_BLOCK:
	case KVIDEO_PALETTE_WRITE:
	case KVIDEO_INDIRECT_WRITE:
		/* Validate before any I/O: a malformed request must cause
		 * no port writes.  The full 16-bit length is streamed. */
		if (payload == (const void *)0 || length == 0u) {
			return (-KOS_EINVAL);
		}
		block.src = (const uint8_t *)payload;
		block.length = length;
		switch (operation) {
		case KVIDEO_CTRL_WRITE:
			v9958_io_stream_ctrl(&block);
			break;
		case KVIDEO_DATA_WRITE:
		case KVIDEO_DATA_BLOCK:
			v9958_io_stream_data(&block);
			break;
		case KVIDEO_PALETTE_WRITE:
			v9958_io_stream_palette(&block);
			break;
		default:
			v9958_io_stream_indirect(&block);
			break;
		}
		return 0;
	case KVIDEO_PRESENT:
		if (length != 0u) {
			return (-KOS_EINVAL);
		}
		/* Flush pending terminal output first: PRESENT is a blocking
		 * point, so nothing of the console stream may stay queued
		 * behind it. */
		v9958_out_flush();
		v9958_io_wait_command();
		return 0;
	case KVIDEO_RESET:
		if (length != 0u) {
			return (-KOS_EINVAL);
		}
		return v9958_display_reset();
	default:
		return (-KOS_ENOTSUP);
	}
}

/* The run-time display reset the native KVIDEO_RESET and the console's
 * reset both run: parser/cursor/rendition defaults, both font atlases
 * reloaded from ROMFS, circular bitmap cleared, cursor re-initialized,
 * present, display enabled. */
int v9958_display_reset(void)
{
	v9958_term_set_defaults();
	if (v9958_init_display() != V9958_OK) {
		return (-KOS_ENODEV);
	}
	return 0;
}

/* The cold display bring-up, shared by the boot segment's initial upload
 * and the run-time KVIDEO_RESET: D0 uses the reset/default interrupt
 * route and every latch write preserves it in the shadow; the terminal
 * defaults are established before the display init so the first clear
 * uses the normal-video background and the cursor SAT matches the home
 * cell.  Returns V9958_OK or V9958_FAIL. */
int v9958_display_bringup(void)
{
	v9958_state.config_shadow = 0x00u;
	v9958_term_set_defaults();
	return v9958_init_display();
}
