/* libgfx private state and the V9958 backend interface.  Not part of
 * the public SDK surface: a program includes only kos/gfx.h.
 *
 * One graphics context per process, held in the one module-level
 * gfx_state.  The whole block is BSS (programs hold no initialized
 * non-const statics), so a closed context is video_handle == 0 (bind
 * returns handles >= 1) and an unset mode is mode_set == 0; the mode
 * geometry follows gfx_set_mode.
 */

#ifndef KOS_SDK_GFX_INTERNAL_H
#define KOS_SDK_GFX_INTERNAL_H

#include "kos/gfx.h"

#define GFX_WIDTH_G6  512u
#define GFX_WIDTH_G7  256u
#define GFX_HEIGHT    212u
#define GFX_BPP_G6    4u
#define GFX_BPP_G7    8u

/* The command queue: one 16-byte record per drawing call, 32 records
 * (512 bytes of the process's data), flushed automatically when full;
 * begin/end nest to at most this depth. */
#define GFX_BATCH_ENTRIES  32u
#define GFX_BATCH_DEPTH_MAX 8u

/* The ellipse expansion's span scratch (gfx_shapes.c): the midpoint
 * walk tracks each scanline's leftmost and rightmost visited dot;
 * the emit pass turns them into one fill span per scanline and the
 * two outline boundary dots. */
#define GFX_SPAN_RECORDS 1u
#define GFX_SPAN_LINES    512u

struct gfx_span_set {
	uint16_t first[GFX_SPAN_LINES];
	uint16_t last[GFX_SPAN_LINES];
	uint16_t count[GFX_SPAN_LINES];
};

/* The record opcodes (struct gfx_cmd.opcode).  FILL and COPY take
 * the GFX_OP_COPY/GFX_OP_XOR operation in `flags`; LINE and PSET
 * carry the VDP op nibble there too (GFX_V9958_OP_*). */
#define GFX_CMD_FILL 1u
#define GFX_CMD_COPY 2u
#define GFX_CMD_LINE 3u
#define GFX_CMD_SPAN 4u   /* one horizontal LMMV run (ellipse pieces) */
#define GFX_CMD_POINT 5u

/* The VDP logical-operation nibbles for the drawing commands: IMP
 * (copy) and EOR (xor).  gfx_op's GFX_OP_COPY is 0 and GFX_OP_XOR 1,
 * so `flags` carries the op nibble directly. */
#define GFX_V9958_OP_IMP 0x0u
#define GFX_V9958_OP_EOR 0x3u

/* The built-in 6x8 CP850 font (gfx_font.c): 256 glyphs, eight row
 * bytes each, active pixels in bits 7..2; gfx_open copies the const
 * table into the RAM font. */
#define GFX_FONT_GLYPH_BYTES   8u
#define GFX_FONT_GLYPH_COUNT   256u
#define GFX_FONT_SIZE          (GFX_FONT_GLYPH_BYTES * GFX_FONT_GLYPH_COUNT)
#define GFX_FONT_GLYPH_WIDTH   6u
#define GFX_FONT_GLYPH_HEIGHT  8u

/* The G6 text expansion's pair cache: this many recent colour pairs,
 * each three 256-entry pages (one per output byte of a glyph row).
 * The size is recorded in the package's validation record. */
#define GFX_TEXT_CACHE_ENTRIES 2u

struct gfx_text_cache_entry {
	uint8_t active;            /* 1 after the first build */
	uint8_t fg;
	uint8_t bg;
	uint8_t pages[3][256];
};

/* One batched drawing call: the design's fixed-size backend-
 * independent record (docs/gfx-sdk.md 9.1). */
struct gfx_cmd {
	uint8_t opcode;
	uint8_t flags;
	uint16_t sx;
	uint16_t sy;
	uint16_t dx;
	uint16_t dy;
	uint16_t width;
	uint16_t height;
	uint8_t color;
	uint8_t reserved;
};

#if defined(__SDCC)
_Static_assert(sizeof(struct gfx_cmd) == 16, "gfx_cmd is 16 bytes");
_Static_assert(GFX_BATCH_ENTRIES == 32, "the batch holds 32 records");
_Static_assert(GFX_FONT_SIZE == 2048, "the font is 2048 bytes");
#endif /* __SDCC */

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

	/* The streaming upload's position (gfx_upload.c). */
	uint8_t upload_active;
	uint8_t upload_full_width;     /* one address covers the transfer */
	uint8_t upload_addressed;      /* the full-width address is set   */
	uint8_t upload_page;           /* y >> 8                          */
	uint8_t upload_row;            /* y & 0xFF                        */
	uint16_t upload_col_byte;      /* the byte column inside the row  */
	uint16_t upload_row_bytes;     /* payload bytes per row           */
	uint16_t upload_row_left;      /* bytes left in the current row   */
	uint16_t upload_remaining;     /* bytes left of the whole upload  */

	/* The drawing batch (gfx_batch.c): begin/end nest to
	 * GFX_BATCH_DEPTH_MAX; a record queued at count ==
	 * GFX_BATCH_ENTRIES flushes the queue first. */
	uint8_t batch_depth;
	uint8_t batch_count;
	struct gfx_cmd batch_queue[GFX_BATCH_ENTRIES];

	/* The RAM font (gfx_font.c): the built-in's mutable copy, made at
	 * gfx_open; the glyph calls change these bytes. */
	uint8_t font_ram[GFX_FONT_SIZE];

	/* The G6 text expansion's pair cache (gfx_text.c): the most
	 * recent colour pairs, round-robin. */
	struct gfx_text_cache_entry text_cache[GFX_TEXT_CACHE_ENTRIES];
	uint8_t text_cache_next;

	/* The ellipse span scratch (gfx_shapes.c): the midpoint walk's
	 * per-scanline bounds and the span submission's staging
	 * record. */
	struct gfx_span_set spans;
	struct gfx_cmd span_record;
};

extern struct gfx_state gfx_state;

/* --- the text path (gfx_text.c) -------------------------------------- */

/* One pixel sink: the expansion hands each glyph row's pixels to the
 * sink, which owns the placement.  `col` and `y` are the row's
 * top-left in the mode's byte units.  The opaque sink writes the bytes
 * through the data port with one VRAM address per glyph row.  A
 * transparent sink plugs in by replacing the emit function: it would
 * start one LMMC per string with TIMP set and R#17's auto-increment
 * off, and feed each expanded row through R#44, the chip skipping
 * colour-0 pixels; the font, the expansion and the text calls stay as
 * they are (docs/gfx-sdk.md 14). */
struct gfx_text_sink {
	void (*emit)(const struct gfx_text_sink *sink, uint16_t col,
		     uint16_t y, const uint8_t *row, uint8_t row_bytes);
};

extern const struct gfx_text_sink gfx_text_sink_opaque;

/* Expand one font row byte to `row_bytes` pixel bytes in the style's
 * colours (3 in G6 through the pair tables, 6 in G7). */
uint8_t gfx_text_expand_row(uint8_t font_row, uint8_t fg, uint8_t bg,
			    uint8_t *row);

/* Install the built-in font into the RAM font (gfx_open calls this,
 * so opening a fresh context always starts from the built-in). */
void gfx_font_install(void);

/* The shape rule shared with gfx_shapes.c: the mode's full width and
 * bounds, and the 512-line space's rows.  A return <= 0 is the result
 * to hand back. */
int gfx_rect_ready(uint16_t x, uint16_t width);
int gfx_rect_check_y(uint16_t y, uint16_t height);

/* --- the shared batch path (gfx_batch.c) ----------------------------- */

/* Append one validated record: executed at once outside a batch,
 * queued inside (the queue flushes itself when full). */
void gfx_batch_submit(const struct gfx_cmd *cmd);

/* --- the V9958 backend (backends/v9958/v9958_gfx.c) ------------------ */

void v9958_set_mode(struct gfx_state *state, enum gfx_mode mode);
void v9958_display_enable(struct gfx_state *state, uint8_t enabled);
void v9958_set_palette(struct gfx_state *state, uint8_t first,
		       uint8_t count, const struct gfx_rgb *colors);
/* Execute `count` queued records in order, each as its own command.
 * The engine is waited for before each one starts; the batch returns
 * with its last command still drawing (asynchronous execution). */
void v9958_run_batch(struct gfx_state *state, const struct gfx_cmd *cmds,
		     uint8_t count);
/* Wait for the command engine (gfx_wait and the CPU VRAM paths). */
void v9958_wait_command(struct gfx_state *state);
/* Wait for the start of the next vertical retrace. */
void v9958_wait_vblank(struct gfx_state *state);
/* Switch the displayed page at the next vertical blank. */
void v9958_show_page(struct gfx_state *state, uint8_t page);

/* --- the backend's port layer (backends/v9958/v9958_io.asm) ----------
 *
 * Every A0h-A4h access the backend makes lives in the assembly file;
 * the ports are the record's, mirrored into these bytes at mode setup,
 * so the OUT forms can use them.  The shape builders build one
 * command block each from a `struct gfx_cmd` and issue it through the
 * indirect port (one R#17 select, then OUTI/OTIR); `g6` tells them
 * the mode, so a GRAPHIC 6 colour packs two 4-bit pixels. */
extern uint8_t gfx_v9958_port_cmd;
extern uint8_t gfx_v9958_port_indirect;
extern uint8_t gfx_v9958_port_data;
extern uint8_t gfx_v9958_port_palette;
extern uint8_t gfx_v9958_g6;
void gfx_v9958_io_reg(uint8_t value, uint8_t reg);
void gfx_v9958_io_wait_command(void);
void gfx_v9958_io_wait_vblank(void);
void gfx_v9958_io_start_command(const uint8_t *cmd);
void gfx_v9958_io_run_hmmv(const struct gfx_cmd *cmd);
void gfx_v9958_io_run_hmmm(const struct gfx_cmd *cmd);
void gfx_v9958_io_run_line(const struct gfx_cmd *cmd);
void gfx_v9958_io_run_pset(const struct gfx_cmd *cmd);
void gfx_v9958_io_run_lmmv(const struct gfx_cmd *cmd);
void gfx_v9958_io_set_vram(uint8_t r14, uint16_t offset);
void gfx_v9958_io_stream_vram(const uint8_t *src, uint16_t length);
void gfx_v9958_io_palette_byte(uint8_t value);

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

/* The VRAM address's second command-port byte sets A16's home in R#14
 * (the 64Kx4 DRAM page). */
#define GFX_V9958_VRAM_ADDR_HI 0x40u

/* The command engine. */
#define GFX_V9958_CMD_REGISTER_BASE 32u
#define GFX_V9958_CMD_REGISTER_COUNT 15u
#define GFX_V9958_CMD_HMMV 0xC0u
#define GFX_V9958_CMD_HMMM 0xD0u
#define GFX_V9958_CMD_LINE 0x70u
#define GFX_V9958_CMD_PSET 0x50u
#define GFX_V9958_CMD_LMMV 0x80u
#define GFX_V9958_STATUS2_CE 0x01u
#define GFX_V9958_STATUS2_VR 0x40u

/* The LINE command's ARG bits: MAJ marks the major axis along Y (the
 * long side is X when clear), DIX/DIY step against the dot order. */
#define GFX_V9958_CMD_MAJ 0x01u

/* The 15-byte command block's offsets (R#32..R#46). */
#define GFX_V9958_CMD_SX_LO  0u
#define GFX_V9958_CMD_SX_HI  1u
#define GFX_V9958_CMD_SY_LO  2u
#define GFX_V9958_CMD_SY_HI  3u
#define GFX_V9958_CMD_DX_LO  4u
#define GFX_V9958_CMD_DX_HI  5u
#define GFX_V9958_CMD_DY_LO  6u
#define GFX_V9958_CMD_DY_HI  7u
#define GFX_V9958_CMD_NX_LO  8u
#define GFX_V9958_CMD_NX_HI  9u
#define GFX_V9958_CMD_NY_LO  10u
#define GFX_V9958_CMD_NY_HI  11u
#define GFX_V9958_CMD_CLR    12u
#define GFX_V9958_CMD_ARG    13u
#define GFX_V9958_CMD_CODE   14u

/* The HMMM argument's direction bits: DIX copies the rows right to
 * left, DIY the columns bottom to top (the block then carries each
 * rectangle's far edge). */
#define GFX_V9958_CMD_DIX 0x04u
#define GFX_V9958_CMD_DIY 0x08u

#endif /* KOS_SDK_GFX_INTERNAL_H */
