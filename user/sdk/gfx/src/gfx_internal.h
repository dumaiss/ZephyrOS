/* libgfx private state and the V9958 backend interface.  Not part of
 * the public SDK surface: a program includes only kos/gfx.h.
 *
 * Every routine that draws or moves pixels is assembly
 * (backends/v9958/gfx_*.asm, docs/gfx-sdk.md 2.6); the C side is the
 * setup: open/close, the mode, the display, the palette, the fonts'
 * tables and the atlas layout.  The drawing state the assembly tests
 * lives in the assembly (gfx_cmd.asm); C writes it at open, mode set and
 * close.
 *
 * One graphics context per process, held in the one module-level
 * gfx_state.  The whole block is BSS (programs hold no initialized
 * non-const statics), so a closed context is video_handle == 0 (bind
 * returns handles >= 1) and an unset mode is mode_set == 0.
 */

#ifndef KOS_SDK_GFX_INTERNAL_H
#define KOS_SDK_GFX_INTERNAL_H

#include "kos/gfx.h"

#define GFX_WIDTH_G6  512u
#define GFX_WIDTH_G7  256u
#define GFX_HEIGHT    212u
#define GFX_BPP_G6    4u
#define GFX_BPP_G7    8u

struct gfx_state {
	int video_handle;              /* 0 while closed, >= 1 the handle */
	uint8_t backend;               /* KVIDEO_BACKEND_*         */
	uint16_t caps;                 /* KVIDEO_* flags           */
	uint8_t mode_set;              /* 1 after gfx_set_mode     */
	uint8_t mode;                  /* enum gfx_mode once set   */
	uint8_t shown_page;            /* R#2's page bit, set at mode/set */
	uint16_t width;
	uint16_t height;
	uint8_t bpp;
	struct kvideo_direct_info info; /* the record gfx_open asked */
};

extern struct gfx_state gfx_state;

/* --- the drawing state (gfx_cmd.asm) --------------------------------- */

extern uint8_t gfx_draw_open;      /* 1 while the handle is held */
extern uint8_t gfx_draw_ready;     /* 1 while open with a mode set */
extern uint8_t gfx_draw_mode;      /* enum gfx_mode */
extern uint16_t gfx_draw_width;    /* the mode's width in dots */
extern uint8_t gfx_draw_shape;     /* 2 G7, 1 G6, 0 no shapes (YJK/none) */
extern uint8_t gfx_batch_depth;    /* gfx_begin nesting */
extern uint8_t gfx_batch_count;    /* queued command blocks */

/* --- the fonts (gfx_font.c, gfx_font4.c) ------------------------------
 *
 * The RAM fonts are row-major: one 256-byte page per glyph row, indexed
 * by the character code, so the text loop reaches a glyph's row with the
 * code alone.  A row byte holds the row's pixels pre-scaled as an index
 * into the colour pair's expansion table: the 6x8's six pixels (bit 5
 * the left) times 4, the 4x6's four pixels (bit 3 the left) times 2.
 * The pages, and the two pages of the expansion tables the text path
 * builds per colour pair, are 256-byte aligned inside one area. */
#define GFX_FONT_GLYPH_BYTES   8u
#define GFX_FONT_GLYPH_COUNT   256u
#define GFX_FONT_SIZE          (GFX_FONT_GLYPH_BYTES * GFX_FONT_GLYPH_COUNT)
#define GFX_FONT_GLYPH_WIDTH   6u
#define GFX_FONT_GLYPH_HEIGHT  8u

/* The const 4x6 table stores glyphs at an 8-byte stride (six rows, two
 * pads), as the asset generator emits it. */
#define GFX_FONT4_GLYPH_BYTES  6u
#define GFX_FONT4_GLYPH_STRIDE 8u
#define GFX_FONT4_SIZE         (GFX_FONT4_GLYPH_STRIDE * GFX_FONT_GLYPH_COUNT)
#define GFX_FONT4_GLYPH_WIDTH  4u
#define GFX_FONT4_GLYPH_HEIGHT 6u

#define GFX_FONT6_PAGES  8u
#define GFX_FONT4_PAGES  6u
#define GFX_PAIR_PAGES   2u
#define GFX_TEXT_PAGES   (GFX_FONT6_PAGES + GFX_FONT4_PAGES + GFX_PAIR_PAGES)

#if defined(__SDCC)
_Static_assert(GFX_FONT_SIZE == 2048, "the font is 2048 bytes");
_Static_assert(GFX_FONT4_SIZE == 2048, "the 4x6 table is 2048 bytes");
#endif /* __SDCC */

/* The pages' high bytes (gfx_font.c sets them at gfx_open). */
extern uint8_t gfx_font6_page;
extern uint8_t gfx_font4_page;
extern uint8_t gfx_pair_page;

/* Install the built-in 6x8 font into its RAM pages (gfx_open calls this,
 * so opening a fresh context always starts from the built-in). */
void gfx_font_install(void);

/* The 4x6 RAM pages, made at the font's first use (the text path calls
 * this), and the glyph calls' bodies. */
void gfx_font4_install(void);
int gfx_font4_set_glyph(uint8_t code, const uint8_t rows[6]);
int gfx_font4_reset_glyph(uint8_t code);
void gfx_font4_reset(void);

/* The 6x8 glyph calls' bodies (gfx_font.c), behind the dispatch in
 * gfx_font4.c. */
int gfx_font_set_glyph6(uint8_t code, const uint8_t rows[8]);
int gfx_font_reset_glyph6(uint8_t code);
void gfx_font_reset6(void);

/* The redefined-glyph marks (gfx_font4.c sets a glyph's bit; the atlas
 * draw clears it once every atlas redraws the cell): one bit per code,
 * per font. */
extern uint8_t gfx_font_stale6[32];
extern uint8_t gfx_font_stale4[32];

/* --- the glyph atlas (gfx_atlas.c lays it out; gfx_atlas_draw.asm) ---- */

#define GFX_ATLAS_SLOTS_MAX 8u
#define GFX_ATLAS_SLOT_FREE 0xFFu

extern uint8_t gfx_atlas_active;   /* 1 after a successful setup */
extern uint8_t gfx_atlas_mode;     /* the setup's mode (packing changes) */
extern uint8_t gfx_atlas_font;     /* GFX_FONT_* */
extern uint8_t gfx_atlas_gw;       /* the glyph width, 4 or 6 dots */
extern uint8_t gfx_atlas_ch;       /* the cell height, 6 or 8 lines */
extern uint8_t gfx_atlas_first;
extern uint16_t gfx_atlas_count;
extern uint8_t gfx_atlas_per_row;  /* glyphs per atlas row */
extern uint8_t gfx_atlas_rows;     /* atlas rows of glyphs */
extern uint8_t gfx_atlas_slots;    /* atlases the band holds */
extern uint8_t gfx_atlas_lru_next;
extern uint8_t gfx_atlas_slot_color[GFX_ATLAS_SLOTS_MAX]; /* FREE or colour */
extern uint8_t gfx_atlas_slot_lru[GFX_ATLAS_SLOTS_MAX];   /* higher: recent */
extern uint16_t gfx_atlas_slot_y[GFX_ATLAS_SLOTS_MAX];    /* each slot's top */
extern uint16_t gfx_atlas_cell_x[256]; /* per range index: the cell's x */
extern uint8_t gfx_atlas_cell_y[256];  /* and its line inside the slot */

int gfx_atlas_setup(enum gfx_font font, uint16_t y, uint16_t lines,
		    uint8_t first, uint16_t count);

/* --- the V9958 backend (backends/v9958/v9958_gfx.c) ------------------ */

void v9958_load_ports(const struct kvideo_direct_info *info);
void v9958_set_mode(struct gfx_state *state, enum gfx_mode mode);
void v9958_display_enable(struct gfx_state *state, uint8_t enabled);
void v9958_set_palette(struct gfx_state *state, uint8_t first,
		       uint8_t count, const struct gfx_rgb *colors);
/* Wait for the command engine (gfx_wait and the setup paths). */
void v9958_wait_command(struct gfx_state *state);
/* Wait for the start of the next vertical retrace. */
void v9958_wait_vblank(struct gfx_state *state);
/* Switch the displayed page at the next vertical blank. */
void v9958_show_page(struct gfx_state *state, uint8_t page);

/* --- the backend's port layer (backends/v9958/v9958_io.asm) ----------
 *
 * The ports are the record's, mirrored into these bytes at gfx_open, so
 * the OUT forms can use them. */
extern uint8_t gfx_v9958_port_cmd;
extern uint8_t gfx_v9958_port_indirect;
extern uint8_t gfx_v9958_port_data;
extern uint8_t gfx_v9958_port_palette;
void gfx_v9958_io_reg(uint8_t value, uint8_t reg);
void gfx_v9958_io_wait_command(void);
void gfx_v9958_io_wait_vblank(void);
void gfx_v9958_io_set_vram(uint8_t r14, uint16_t offset);
void gfx_v9958_io_stream_vram(const uint8_t *src, uint16_t length);
void gfx_v9958_io_palette_byte(uint8_t value);
/* Lay a const font out in its row-major RAM pages (the 6x8 or the 4x6
 * row byte encoding above). */
void gfx_font_transpose6(uint8_t page, const uint8_t *src);
void gfx_font_transpose4(uint8_t page, const uint8_t *src);

/* V9958 register numbers (V9958 facts, private to the backend). */
#define GFX_V9958_REG_R0  0u
#define GFX_V9958_REG_R1  1u
#define GFX_V9958_REG_R2  2u
#define GFX_V9958_REG_R8  8u
#define GFX_V9958_REG_R15 15u
#define GFX_V9958_REG_R16 16u
#define GFX_V9958_REG_R17 17u
#define GFX_V9958_REG_R25 25u
#define GFX_V9958_REG_R46 46u

#define GFX_V9958_REG_SELECT 0x80u

/* Register values every mode establishes, from the proven bring-up
 * baseline (drivers/v9958/v9958.c): page-zero addressing, 64Kx4 DRAMs,
 * 212 non-interlaced lines, sprites off, status register 2 selected,
 * display offset 0, WAIT on. */
#define GFX_V9958_R0_G6        0x0Au
#define GFX_V9958_R0_G7        0x0Eu
#define GFX_V9958_R1_DISPLAY_OFF 0x00u
#define GFX_V9958_R1_DISPLAY_ON  0x40u
#define GFX_V9958_R2_PAGE_ZERO 0x1Fu
#define GFX_V9958_R2_PAGE_BIT  0x20u
#define GFX_V9958_R8_SPRITES_OFF 0x02u
#define GFX_V9958_R8_64K_DRAM  0x08u
#define GFX_V9958_R9_212       0x80u
#define GFX_V9958_R15_STATUS2  0x02u
#define GFX_V9958_R25_WAIT_ON  0x04u
#define GFX_V9958_R25_YJK      0x08u /* R#25 bit 3; bit 4 (10h) is YAE */

/* The command engine (the drawing assembly holds the command codes). */
#define GFX_V9958_STATUS2_CE 0x01u
#define GFX_V9958_STATUS2_VR 0x40u

#endif /* KOS_SDK_GFX_INTERNAL_H */
