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

/* gfx_clear's fill height: the visible 212 lines of page 0, then the
 * 256-line page stride, then page 1's 212 visible lines, so one HMMV
 * clears the visible area of both pages. */
#define GFX_CLEAR_LINES (GFX_HEIGHT + 256u)

struct gfx_state {
	int video_handle;              /* 0 while closed, >= 1 the handle */
	uint8_t backend;               /* KVIDEO_BACKEND_*         */
	uint16_t caps;                 /* KVIDEO_* flags           */
	uint8_t mode_set;              /* 1 after gfx_set_mode     */
	uint8_t mode;                  /* enum gfx_mode once set   */
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
};

extern struct gfx_state gfx_state;

/* --- the V9958 backend (backends/v9958/v9958_gfx.c) ------------------ */

void v9958_set_mode(struct gfx_state *state, enum gfx_mode mode);
void v9958_display_enable(struct gfx_state *state, uint8_t enabled);
void v9958_clear(struct gfx_state *state, gfx_color_t color);
void v9958_set_palette(struct gfx_state *state, uint8_t first,
		       uint8_t count, const struct gfx_rgb *colors);

/* --- the backend's port layer (backends/v9958/v9958_io.asm) ----------
 *
 * Every A0h-A4h access the backend makes lives in the assembly file;
 * the ports are the record's, mirrored into these bytes at mode setup,
 * so the OUT forms can use them. */
extern uint8_t gfx_v9958_port_cmd;
extern uint8_t gfx_v9958_port_indirect;
extern uint8_t gfx_v9958_port_data;
extern uint8_t gfx_v9958_port_palette;
void gfx_v9958_io_reg(uint8_t value, uint8_t reg);
void gfx_v9958_io_wait_command(void);
void gfx_v9958_io_start_command(const uint8_t *cmd);
void gfx_v9958_io_set_vram(uint8_t r14, uint16_t offset);
void gfx_v9958_io_stream_vram(const uint8_t *src, uint16_t length);
void gfx_v9958_io_palette_byte(uint8_t value);

/* V9958 register numbers (V9958 facts, private to the backend). */
#define GFX_V9958_REG_R0  0u
#define GFX_V9958_REG_R1  1u
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
#define GFX_V9958_R8_SPRITES_OFF 0x02u
#define GFX_V9958_R8_64K_DRAM  0x08u
#define GFX_V9958_R9_212       0x80u
#define GFX_V9958_R15_STATUS2  0x02u
#define GFX_V9958_R25_WAIT_ON  0x04u
#define GFX_V9958_R25_YJK      0x08u /* R#25 bit 3; bit 4 (10h) is YAE */

/* The YJK mode's shared luminance: on the V9958 the YJK pixel carries
 * J and K (3 bits each) and Y comes from palette entry 0, so the mode
 * setup programmes entry 0 to this grey and tools/kimg.py encodes
 * against the same value. */
#define GFX_V9958_YJK_Y   24u
#define GFX_V9958_YJK_Y3  (GFX_V9958_YJK_Y >> 2)

/* The VRAM address's second command-port byte sets A16's home in R#14
 * (the 64Kx4 DRAM page). */
#define GFX_V9958_VRAM_ADDR_HI 0x40u

/* The command engine. */
#define GFX_V9958_CMD_REGISTER_BASE 32u
#define GFX_V9958_CMD_REGISTER_COUNT 15u
#define GFX_V9958_CMD_HMMV 0xC0u
#define GFX_V9958_STATUS2_CE 0x01u

/* The 15-byte command block's offsets (R#32..R#46). */
#define GFX_V9958_CMD_DX_LO  4u
#define GFX_V9958_CMD_DX_HI  5u
#define GFX_V9958_CMD_DY_LO  6u
#define GFX_V9958_CMD_DY_HI  7u
#define GFX_V9958_CMD_NX_LO  8u
#define GFX_V9958_CMD_NX_HI  9u
#define GFX_V9958_CMD_NY_LO  10u
#define GFX_V9958_CMD_NY_HI  11u
#define GFX_V9958_CMD_CLR    12u
#define GFX_V9958_CMD_CODE   14u

#endif /* KOS_SDK_GFX_INTERNAL_H */
