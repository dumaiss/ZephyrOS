/* Internal interface of the V9958 video driver (drivers/v9958/).
 *
 * v9958.c owns the GRAPHIC 6 display state, the ported initialization
 * and command sequencing, the font-atlas upload, and the output-only
 * 85x26 text terminal renderer; v9958_io.asm owns every port access and
 * the timing-critical streaming loops; v9958_boot.c owns the boot-tier
 * bring-up and v9958_service.c the resident-service protocol dispatch.
 * The driver reaches the nucleus only through the service bootstrap
 * interface (include/zephyros/ipc.h); its bulk font lives in bank 6's
 * boot/bulk tier and is re-read through the nucleus bulk copy.  Not a
 * kernel ABI.
 *
 * The ported behavior is the hardware-proven 85x26, 6x8 CP850 GRAPHIC 6
 * bring-up of the pinned Zephyr-80 reference (LunchCrema V9958,
 * commit 2c6b9650): software-paced bootstrap with the front porch
 * bypassed, native WAIT from R#25.WTE=1 on, palette programmed only
 * after the porch is enabled, both precolored font atlases uploaded
 * from the reference font, circular page-zero bitmap cleared, sprite
 * cursor initialized, then the display enabled.  The VDP interrupt is
 * never enabled in M8.
 *
 * The terminal is a straight port of the reference's ANSI/VT100-light
 * CONOUT parser and drawing algorithms with the CP/M dispatch table,
 * BIOS addresses, HID coupling and historical packet ceiling removed.
 * It is strictly output-only: bytes arrive through
 * v9958_char_putc()/v9958_char_write(), and nothing here knows about
 * SIO, IOC or HID.
 */

#ifndef ZEPHYROS_DRIVERS_V9958_DRIVER_H
#define ZEPHYROS_DRIVERS_V9958_DRIVER_H

#include "zephyros/ipc.h"
#include "zos/protocol.h"

/* --- GRAPHIC 6 layout (hardware-proven 85x26 console baseline) ---------
 *
 * 512x256-byte page-zero bitmap (256 scanlines of one 256-byte G6 pitch),
 * 85 columns of 6-pixel cells, 26 rows of 8-line cells, display offset
 * 4 source lines.  Only the layout constants the core owns in package
 * 18 are declared here; the character renderer/terminal arrives with the
 * console package. */

#define V9958_ATLAS_SCANLINES   64u  /* 8 atlas rows x 8 glyph lines      */
#define V9958_ATLAS_PITCH       256u /* one G6 scanline (512 px, 4bpp)    */
#define V9958_ATLAS_GLYPHS      8u   /* glyphs per staged atlas segment   */
#define V9958_ATLAS_SEG_BYTES   24u  /* 8 glyphs x 6 pixels / 2 per byte  */
#define V9958_ATLAS_CHUNKS      32u  /* 256 glyphs / 8 per fetched chunk  */
#define V9958_GLYPH_BYTES       8u   /* 6x8 font: 8 rows per glyph        */

/* --- 85x26 6x8 output terminal (ported reference renderer) ------------
 *
 * The terminal state machine, CR/LF/BS/TAB/FF handling, ANSI/VT100-light
 * CSI movement/erase/SGR/save-restore/DECAWM/IL/DL behavior, the
 * circular R#23 scrolling and the sprite cursor are ported one-to-one
 * from the pinned reference implementation.  It is output-only: bytes
 * arrive exclusively through the generic character interface. */
#define V9958_COLUMNS        85u  /* logical text columns                 */
#define V9958_ROWS           26u  /* logical text rows                    */
#define V9958_CELL_WIDTH     6u   /* pixels per character cell            */
#define V9958_CELL_HEIGHT    8u   /* scanlines per character cell         */
#define V9958_DISPLAY_OFFSET 4u   /* fixed lines above logical row zero   */
#define V9958_OUT_RUN_SIZE   64u  /* print-run batching bound (bytes)     */
#define V9958_SCROLL_BOTTOM  (V9958_ROWS - 1u)
#define V9958_PIXEL_WIDTH    (V9958_COLUMNS * V9958_CELL_WIDTH)

/* Output parser states (ported ESC/CSI state machine). */
#define V9958_TERM_NORMAL    0u
#define V9958_TERM_ESC       1u
#define V9958_TERM_CSI       2u
#define V9958_TERM_ESC_HASH  3u  /* ESC # x: consume one final byte      */
#define V9958_TERM_CHARSET   4u  /* ESC ( ) O x: consume one final byte  */

/* Atlas placement in 128-KiB VRAM: normal at 10000h, reverse at 14000h,
 * each scanline one 256-byte pitch apart.  A scanline carries 32 glyphs
 * of 3 precolored bytes (96 meaningful bytes), written as four 24-byte
 * segments staged from one 8-glyph font chunk at a time. */
#define V9958_ATLAS_NORMAL_A16  1u
#define V9958_ATLAS_NORMAL_ADDR 0x0000u
#define V9958_ATLAS_REVERSE_A16 1u
#define V9958_ATLAS_REVERSE_ADDR 0x4000u

/* Font resource carried as ROMFS DATA (never resident module storage). */
#define V9958_FONT_NAME   "v9958-6x8.font"
#define V9958_FONT_SIZE   2048u
#define V9958_FONT_CHUNK  64u   /* one 8-glyph chunk per bounded read */

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
#define V9958_FILL_COLOR_REVERSE 0xFFu /* white background (reverse video) */

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

/* Terminal byte entries: assembly wrappers that run the C renderer
 * workers on a private service stack (the kernel syscall gate stack is
 * too small for the renderer's call chain).  Prototypes are the exact
 * sdcccall(1) shapes the service dispatch calls. */
int v9958_char_putc(void *state, char ch);
int v9958_char_write(void *state, const char *buf, uint16_t len);

/* Driver state: module BSS, relocated with the module image. */
struct v9958_state {
	uint8_t config_shadow;         /* latch shadow; D0 always preserved */
	uint8_t scroll_origin;         /* physical scanline of row zero    */

	/* --- terminal parser (ported state machine) --- */
	uint8_t term_state;      /* V9958_TERM_*                        */
	uint8_t esc_count;       /* triple-ESC display reset counter    */
	uint8_t csi_private;     /* nonzero after a CSI '?' prefix      */
	uint8_t csi_param_count; /* stored numeric parameters (max 2)   */
	uint8_t csi_accum;       /* parameter being accumulated         */
	uint8_t csi_param0;
	uint8_t csi_param1;

	/* --- cursor and rendition --- */
	uint8_t text_col;        /* internal 0-based logical column     */
	uint8_t text_row;        /* internal 0-based logical row        */
	uint8_t cursor_visible;  /* sprite cursor shown                 */
	uint8_t current_attr;    /* SGR attribute; bit 0 = reverse      */
	uint8_t saved_col;       /* ESC 7 / CSI s saved state           */
	uint8_t saved_row;
	uint8_t saved_attr;
	uint8_t auto_wrap;       /* DECAWM; 1 = enabled (default)       */

	/* --- print-run batching (64 bytes, reference policy) --- */
	uint8_t out_run_count;
	uint8_t out_run_col;     /* origin cell of the pending run      */
	uint8_t out_run_row;
};

/* Internal result convention of the bring-up chain (the value pair the
 * retired module ABI used). */
#define V9958_OK   0
#define V9958_FAIL (-1)

/* The shared display bring-up (full initialization incl. the font
 * upload), used by the boot segment's initial pass and by the run-time
 * ZVIDEO_RESET.  Returns ZDRIVER_OK or ZDRIVER_FAIL. */
int v9958_display_bringup(void);

/* The boot-tier direct font source: the boot entry leaves the font's
 * bank-6 address here for the initial upload; the service init clears it
 * so every later read goes through the nucleus bulk copy. */
void v9958_set_font_base(uint16_t base);
void v9958_set_api(const struct ipc_service_api *api);

/* Boot segment: the initial bring-up, run once through the mode-10 gate. */
void v9958_boot_entry(void);

/* Hot segment: the endpoint dispatch and the init entry the loader calls
 * with the service bootstrap table. */
int16_t v9958_service_dispatch(struct zos_ipc_frame *frame);
void v9958_service_init(const struct ipc_service_boot *boot);

/* The class operation implementation (exposed for the descriptor).  A
 * ZVIDEO_RESET reinitializes the terminal parser, cursor/rendition
 * defaults, every VDP-owned register and selector, both font atlases,
 * the circular bitmap and the sprite cursor, then presents and leaves
 * the display enabled. */
int v9958_send(void *state, uint8_t operation, const void *payload,
	       uint16_t length);

extern struct v9958_state v9958_state;
extern volatile uint8_t v9958_boot_runs;

#endif /* ZEPHYROS_DRIVERS_V9958_DRIVER_H */
