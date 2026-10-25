/* libgfx V9958 backend (user/sdk/gfx/src/backends/v9958/).
 *
 * Mode setup, the display switch and the primitives' command engine,
 * in C over the assembly port layer (v9958_io.asm owns every A0h-A4h
 * access).  The ports come from the KVIDEO_OP_DIRECT_INFO record, not
 * from board constants; the register numbers are V9958 facts.
 *
 * gfx_set_mode's register set mirrors the proven bring-up baseline
 * (drivers/v9958/v9958.c v9958_init_g6) through native-WAIT writes:
 * stop any command left running, R#8 = 64Kx4 DRAM with sprites off,
 * then R#0..R#27 in the baseline's values - every mode 212 lines,
 * non-interlaced (R#9 = 80h), display off (R#1 = 00h, IE0 off), status
 * register 2 selected, display offset 0 - and R#25 last with WTE on
 * (plus YJK for the YJK mode).  The release's display init guarantees
 * the hardware WAIT is on, so no write is software-paced.
 *
 * The tables are const (a program cannot hold an initialized
 * non-const static array); the command blocks are built on the stack.
 */

#include "kos/gfx.h"
#include "gfx_internal.h"

/* The common register values, in write order (value, register): R#1
 * (display off, IE0 off), page-zero addressing, the base-table zeros,
 * the border, R#9's 212 non-interlaced lines, the selectors and R#23's
 * display offset.  R#8, R#0 and R#25 are per-mode and written around
 * it. */
static const uint8_t v9958_common_regs[][2] = {
	{GFX_V9958_R1_DISPLAY_OFF, GFX_V9958_REG_R1},
	{GFX_V9958_R2_PAGE_ZERO, 2u},
	{0x00u, 3u},
	{0x00u, 4u},
	{0x00u, 5u},
	{0x00u, 6u},
	{0x00u, 7u},                  /* black border              */
	{GFX_V9958_R9_212, 9u},
	{0x00u, 10u},
	{0x00u, 11u},
	{0x00u, 12u},
	{0x00u, 13u},
	{0x00u, 14u},                 /* A16..A14 = 0              */
	{GFX_V9958_R15_STATUS2, GFX_V9958_REG_R15},
	{0x00u, 16u},
	{0x00u, 17u},
	{0x00u, 18u},
	{0x00u, 19u},
	{0x00u, 23u},                 /* display offset 0          */
	{0x00u, 26u},
	{0x00u, 27u},
};
#define V9958_COMMON_REG_COUNT \
	(sizeof(v9958_common_regs) / sizeof(v9958_common_regs[0]))

/* Load the ports the record reported into the assembly layer's bytes:
 * the backend reaches the command, indirect, data and palette ports
 * through them. */
static void v9958_load_ports(const struct kvideo_direct_info *info)
{
	gfx_v9958_port_cmd = info->command_port;
	gfx_v9958_port_indirect = info->indirect_port;
	gfx_v9958_port_data = info->data_port;
	gfx_v9958_port_palette = info->palette_port;
}

void v9958_set_mode(struct gfx_state *state, enum gfx_mode mode)
{
	uint8_t r0 = (mode == GFX_MODE_512x212_16) ?
		GFX_V9958_R0_G6 : GFX_V9958_R0_G7;
	uint8_t r25 = GFX_V9958_R25_WAIT_ON;
	uint8_t i;

	if (mode == GFX_MODE_256x212_YJK) {
		r25 |= GFX_V9958_R25_YJK;
	}
	v9958_load_ports(&state->info);

	/* A previous holder may have left a command running. */
	gfx_v9958_io_reg(0x00u, GFX_V9958_REG_R46);
	gfx_v9958_io_reg(GFX_V9958_R8_64K_DRAM | GFX_V9958_R8_SPRITES_OFF,
			 GFX_V9958_REG_R8);
	gfx_v9958_io_reg(r0, GFX_V9958_REG_R0);
	for (i = 0u; i < V9958_COMMON_REG_COUNT; ++i) {
		gfx_v9958_io_reg(v9958_common_regs[i][0],
				 v9958_common_regs[i][1]);
	}
	gfx_v9958_io_reg(r25, GFX_V9958_REG_R25);
}

void v9958_display_enable(struct gfx_state *state, uint8_t enabled)
{
	v9958_load_ports(&state->info);
	gfx_v9958_io_reg(enabled != 0u ? GFX_V9958_R1_DISPLAY_ON :
			 GFX_V9958_R1_DISPLAY_OFF, GFX_V9958_REG_R1);
}

/* Adjust one HMMM record in place before the assembly builder: when
 * the source sits left of (or above) the destination the overlapping
 * rows (or columns) must copy from the far edge, so DIX (or DIY) is
 * set in flags and sx/sy/dx/dy carry each rectangle's far edge;
 * otherwise the near edge and the forward direction.  Coordinates
 * are dots, 9-bit for the 512-dot axis and the 512-line space. */
static void v9958_hmmm_adjust(struct gfx_cmd *record)
{
	if (record->sx < record->dx) {
		record->flags |= GFX_V9958_CMD_DIX;
		record->sx = (uint16_t)(record->sx + record->width - 1u);
		record->dx = (uint16_t)(record->dx + record->width - 1u);
	}
	if (record->sy < record->dy) {
		record->flags |= GFX_V9958_CMD_DIY;
		record->sy = (uint16_t)(record->sy + record->height - 1u);
		record->dy = (uint16_t)(record->dy + record->height - 1u);
	}
}

/* Execute `count` records in order: one command per record, each one
 * built and started by the assembly builders (the block build and the
 * R#32-46 write live there: one R#17 select, then OUTI/OTIR), and
 * waited for, so the caller observes the batch exactly as queued. */
void v9958_run_batch(struct gfx_state *state, const struct gfx_cmd *cmds,
		     uint8_t count)
{
	struct gfx_cmd record;
	uint8_t i;

	gfx_v9958_g6 = state->mode == (uint8_t)GFX_MODE_512x212_16 ? 1u : 0u;
	v9958_load_ports(&state->info);
	for (i = 0u; i < count; ++i) {
		switch (cmds[i].opcode) {
		case GFX_CMD_FILL:
			if (cmds[i].reserved != 0u) {
				gfx_v9958_io_run_lmmv(&cmds[i]);
			} else {
				gfx_v9958_io_run_hmmv(&cmds[i]);
			}
			break;
		case GFX_CMD_COPY:
			record = cmds[i];
			v9958_hmmm_adjust(&record);
			gfx_v9958_io_run_hmmm(&record);
			break;
		case GFX_CMD_LINE:
			gfx_v9958_io_run_line(&cmds[i]);
			break;
		case GFX_CMD_SPAN:
			gfx_v9958_io_run_lmmv(&cmds[i]);
			break;
		default:
			gfx_v9958_io_run_pset(&cmds[i]);
			break;
		}
		gfx_v9958_io_wait_command();
	}
}

/* The palette, only in the 16-colour mode (gfx_palette.c refuses the
 * others): for each entry the V9958 representation is (R3 << 4) | B3
 * then G3, the entry selected through R#16 and the palette pointer
 * auto-advancing after every pair. */
void v9958_set_palette(struct gfx_state *state, uint8_t first,
		       uint8_t count, const struct gfx_rgb *colors)
{
	uint8_t i;

	v9958_load_ports(&state->info);
	for (i = 0u; i < count; ++i) {
		gfx_v9958_io_reg((uint8_t)(first + i), GFX_V9958_REG_R16);
		gfx_v9958_io_palette_byte((uint8_t)(((colors[i].r >> 5) << 4) |
						    (colors[i].b >> 5)));
		gfx_v9958_io_palette_byte((uint8_t)(colors[i].g >> 5));
	}
}
