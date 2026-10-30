#include "kos.h"
#include "kos/gfx.h"
#include "view.h"

#define PAGE_Y(page) ((uint16_t)(page) * 256u)
#define MINOR_HILITE 4u
#define MAJOR_HILITE 16u

#define X_A1 14u
#define X_A2 50u
#define X_A3 86u
#define X_AN 124u
#define X_B1 138u
#define X_B2 174u
#define X_B3 210u
#define X_BN 248u
#define X_C1 262u
#define X_C2 298u
#define X_C3 334u
#define X_CN 372u
#define X_D1 386u
#define X_D2 422u
#define X_D3 458u
#define X_DN 496u

_Static_assert((X_A1 | X_A2 | X_A3 | X_AN | X_B1 | X_B2 | X_B3 |
		X_BN | X_C1 | X_C2 | X_C3 | X_CN | X_D1 | X_D2 |
		X_D3 | X_DN) % 2u == 0u, "GRAPHIC 6 text x must be even");

static const uint16_t cell_x[ZTRDOC_CHANNELS] = {
	X_A1, X_A2, X_A3, X_AN, X_B1, X_B2, X_B3, X_BN,
	X_C1, X_C2, X_C3, X_CN, X_D1, X_D2, X_D3, X_DN
};

static const struct gfx_rgb sntrack_palette[16] = {
	{0u,0u,36u}, {36u,36u,72u}, {72u,72u,109u}, {145u,145u,182u},
	{218u,218u,218u}, {182u,182u,255u}, {109u,182u,182u}, {218u,182u,109u},
	{72u,36u,145u}, {109u,72u,182u}, {145u,109u,218u}, {182u,182u,218u},
	{255u,255u,255u}, {218u,218u,255u}, {182u,255u,218u}, {255u,218u,145u}
};

static struct view *active_view;
static struct ztrdoc_event decoded[ZTRDOC_CHANNELS][ZTRDOC_VIEW_ROWS];
static struct gfx_text_style text_style;
/* CP850 FAh, the middle dot: an empty character slot. */
static const char dots[27] = {
	'\xFA','\xFA','\xFA','\xFA','\xFA','\xFA','\xFA','\xFA','\xFA',
	'\xFA','\xFA','\xFA','\xFA','\xFA','\xFA','\xFA','\xFA','\xFA',
	'\xFA','\xFA','\xFA','\xFA','\xFA','\xFA','\xFA','\xFA','\xFA'
};

static char hex_digit(uint8_t value)
{
	value &= 15u;
	return (char)(value < 10u ? '0' + value : 'A' + value - 10u);
}

static void hex2(char out[3], uint8_t value)
{
	out[0] = hex_digit((uint8_t)(value >> 4));
	out[1] = hex_digit(value); out[2] = '\0';
}

static void decimal(char *out, uint16_t value)
{
	char reverse[6]; uint8_t n = 0u, i;
	do { reverse[n++] = (char)('0' + value % 10u); value /= 10u; }
	while (value != 0u);
	for (i = 0u; i < n; ++i) out[i] = reverse[n - i - 1u];
	out[n] = '\0';
}

static int draw_text(uint16_t x, uint16_t y, const char *s, uint16_t n,
		     uint8_t fg, uint8_t bg)
{
	text_style.fg = fg; text_style.bg = bg;
	text_style.flags = 0u; text_style.font = GFX_FONT_4X6;
	return gfx_text_n(x, y, s, n, &text_style);
}

static int text(uint16_t x, uint16_t y, const char *s, uint8_t fg, uint8_t bg)
{
	return draw_text(x, y, s, strlen(s), fg, bg);
}

static void draw_title(const struct view *v, uint8_t page)
{
	char b[10]; uint16_t mhz100; uint16_t y = PAGE_Y(page);
	(void)gfx_fill_rect(0u, y, 512u, 9u, 1u, GFX_OP_COPY);
	(void)text(0u, y + 1u, "SN-TRACK", 5u, 1u);
	(void)draw_text(38u, y + 1u, v->doc->title, strlen(v->doc->title), 4u, 1u);
	(void)text(180u, y + 1u, "4 X SN76489", 3u, 1u);
	mhz100 = (uint16_t)((v->doc->clock_hz + 5000ul) / 10000ul);
	b[0] = (char)('0' + mhz100 / 100u); b[1] = '.';
	b[2] = (char)('0' + (mhz100 / 10u) % 10u); b[3] = (char)('0' + mhz100 % 10u);
	b[4] = 'M'; b[5] = 'H'; b[6] = 'Z'; b[7] = '\0';
	(void)text(268u, y + 1u, b, 4u, 1u);
	decimal(b, v->doc->rate); (void)text(320u, y + 1u, b, 4u, 1u);
	(void)text((uint16_t)(320u + strlen(b) * 4u), y + 1u, "HZ", 4u, 1u);
	(void)text(356u, y + 1u, "STOP", 7u, 1u);
	(void)text(492u, y + 1u, "00:00", 4u, 1u);
}

static void draw_orders(const struct view *v, uint8_t page)
{
	uint16_t y = PAGE_Y(page), order; uint8_t line, ch; char b[4];
	(void)gfx_fill_rect(0u, y + 9u, 216u, 35u, 1u, GFX_OP_COPY);
	(void)text(0u, y + 10u,
		"POS A1 A2 A3 AN B1 B2 B3 BN C1 C2 C3 CN D1 D2 D3 DN", 3u, 1u);
	for (line = 0u; line < 4u; ++line) {
		order = v->order_top + line;
		if (order >= v->doc->order_count) continue;
		hex2(b, (uint8_t)order); b[2] = ' '; b[3] = '\0';
		(void)draw_text(0u, y + 19u + line * 6u, b, 3u, 4u, 1u);
		for (ch = 0u; ch < 16u; ++ch) {
			if (v->doc->orders[order][ch] == 0xFFu) continue;
			hex2(b, v->doc->orders[order][ch]);
			(void)draw_text(12u + (uint16_t)ch * 12u,
				y + 19u + line * 6u, b, 2u, 4u, 1u);
		}
		if (order == v->order)
			(void)gfx_fill_rect(0u, y + 19u + line * 6u, 204u, 6u,
					    8u, GFX_OP_XOR);
	}
}

static void draw_instruments(const struct view *v, uint8_t page)
{
	uint16_t y = PAGE_Y(page); uint8_t line; char b[21];
	(void)gfx_fill_rect(216u, y + 9u, 130u, 35u, 1u, GFX_OP_COPY);
	(void)text(218u, y + 10u, "INSTRUMENT", 3u, 1u);
	for (line = 0u; line < 4u && v->instrument_top + line < v->doc->instrument_count; ++line) {
		const struct ztrdoc_instrument *ins =
			&v->doc->instruments[v->instrument_top + line];
		uint8_t n = 3u, k = 0u;
		hex2(b, ins->number); b[2] = ' ';
		while (k < 15u && ins->name[k] != '\0') b[n++] = ins->name[k++];
		while (n < 18u) b[n++] = ' ';
		b[n++] = ins->type == ZTRDOC_INSTR_TONE ? 'T' :
			 ins->type == ZTRDOC_INSTR_NOISE ? 'N' : 'M'; b[n] = '\0';
		(void)draw_text(218u, y + 19u + line * 6u, b, n,
			ins->type == ZTRDOC_INSTR_UNUSED ? 7u : 4u, 1u);
		if (ins->number == 0u)
			(void)gfx_fill_rect(216u, y + 19u + line * 6u, 130u, 6u,
					    8u, GFX_OP_XOR);
	}
}

static void draw_song(const struct view *v, uint8_t page)
{
	uint16_t y = PAGE_Y(page), tempo; char b[12], n[6];
	(void)gfx_fill_rect(346u, y + 9u, 166u, 35u, 1u, GFX_OP_COPY);
	(void)text(348u, y + 10u, "SONG", 3u, 1u);
	(void)text(348u, y + 16u, "TITLE", 3u, 1u);
	(void)draw_text(372u, y + 16u, v->doc->title,
			strlen(v->doc->title), 4u, 1u);
	(void)text(348u, y + 22u, "SPEED", 3u, 1u); hex2(n, v->doc->speed);
	(void)draw_text(372u, y + 22u, n, 2u, 4u, 1u);
	tempo = (uint16_t)(((uint16_t)15u * v->doc->rate + v->doc->speed / 2u) /
			    v->doc->speed);
	(void)text(390u, y + 22u, "TEMPO", 3u, 1u); decimal(n, tempo);
	(void)text(414u, y + 22u, n, 4u, 1u);
	(void)text(348u, y + 28u, "ROWS", 3u, 1u);
	if (v->doc->rows == 256u) { n[0]='1'; n[1]='0'; n[2]='0'; n[3]='\0'; }
	else hex2(n, (uint8_t)v->doc->rows);
	(void)text(368u, y + 28u, n, 4u, 1u);
	(void)text(390u, y + 28u, "HILITE", 3u, 1u);
	(void)text(418u, y + 28u, "04/10", 4u, 1u);
	(void)text(348u, y + 34u, "POS", 3u, 1u); hex2(b, (uint8_t)v->order);
	b[2]='/'; hex2(b + 3u, (uint8_t)v->doc->order_count);
	(void)draw_text(364u, y + 34u, b, 5u, 4u, 1u);
	(void)text(390u, y + 34u, "PAT", 3u, 1u);
}

static void draw_tone(uint16_t x, uint16_t y,
		      const struct ztrdoc_event *event, uint8_t bg)
{
	static const char notes[] = "C-C#D-D#E-F-F#G-G#A-A#B-";
	char b[10]; uint8_t pitch, octave;
	memset(b, ' ', 9u); b[9] = '\0';
	if ((event->mask & 0x0Cu) != 0u) b[0] = b[1] = b[2] = '=';
	else if ((event->mask & 0x80u) != 0u) {
		pitch = event->note; octave = 0u;
		while (pitch >= 12u) { pitch = (uint8_t)(pitch - 12u); ++octave; }
		b[0] = notes[pitch * 2u]; b[1] = notes[pitch * 2u + 1u];
		b[2] = (char)('0' + octave);
	}
	if ((event->mask & 0x40u) != 0u) { b[3]=hex_digit(event->instrument >> 4); b[4]=hex_digit(event->instrument); }
	if ((event->mask & 0x20u) != 0u) b[5]=hex_digit(event->volume);
	if ((event->mask & 0x10u) != 0u) { b[6]=hex_digit(event->effect); b[7]=hex_digit(event->parameter >> 4); b[8]=hex_digit(event->parameter); }
	if ((event->mask & 0x8Cu) != 0u)
		(void)draw_text(x, y, b, 3u, 4u, bg);
	if ((event->mask & 0x40u) != 0u)
		(void)draw_text(x + 12u, y, b + 3u, 2u, 5u, bg);
	if ((event->mask & 0x20u) != 0u)
		(void)draw_text(x + 20u, y, b + 5u, 1u, 6u, bg);
	if ((event->mask & 0x10u) != 0u)
		(void)draw_text(x + 24u, y, b + 6u, 3u, 7u, bg);
}

static void draw_noise(uint16_t x, uint16_t y,
		       const struct ztrdoc_event *event, uint8_t bg)
{
	char b[4] = {' ',' ',' ','\0'}; uint8_t shown, control;
	control = ztrdoc_noise_control(active_view->doc, event, &shown);
	if (shown) b[0] = hex_digit(control);
	if ((event->mask & 0x40u) != 0u) b[1] = hex_digit(event->instrument);
	if ((event->mask & 0x20u) != 0u) b[2] = hex_digit(event->volume);
	if (shown) (void)draw_text(x, y, b, 1u, 4u, bg);
	if ((event->mask & 0x40u) != 0u)
		(void)draw_text(x + 4u, y, b + 1u, 1u, 5u, bg);
	if ((event->mask & 0x20u) != 0u)
		(void)draw_text(x + 8u, y, b + 2u, 1u, 6u, bg);
}

int view_draw_row(uint8_t page, uint16_t row,
		  const struct ztrdoc_event events[ZTRDOC_CHANNELS])
{
	uint16_t y, actual; uint8_t channel, bg; char b[4];
	if (active_view == 0 || page > 1u || row >= 25u || events == 0) return -1;
	actual = active_view->first_row + row; y = PAGE_Y(page) + 55u + row * 6u;
	bg = actual < active_view->doc->rows && actual % MINOR_HILITE == 0u ? 1u : 0u;
	(void)gfx_fill_rect(0u, y, 512u, 6u, bg, GFX_OP_COPY);
	if (actual >= active_view->doc->rows) return 0;
	hex2(b, (uint8_t)actual);
	(void)draw_text(0u, y, b, 2u, actual % MAJOR_HILITE == 0u ? 5u : 3u, bg);
	/* Every slot starts as a dot; present fields are drawn over it. */
	for (channel = 0u; channel < 4u; ++channel) {
		(void)draw_text(cell_x[channel * 4u], y, dots, 27u, 2u, bg);
		(void)draw_text(cell_x[channel * 4u + 3u], y, dots, 3u, 2u, bg);
	}
	for (channel = 0u; channel < 16u; ++channel) {
		if ((channel & 3u) == 3u) draw_noise(cell_x[channel], y, &events[channel], bg);
		else draw_tone(cell_x[channel], y, &events[channel], bg);
	}
	return 0;
}

static void draw_headers(const struct view *v, uint8_t page)
{
	uint16_t y = PAGE_Y(page); uint8_t ch; char b[2];
	(void)gfx_fill_rect(0u, y + 44u, 512u, 11u, 0u, GFX_OP_COPY);
	for (ch = 0u; ch < 16u; ++ch) {
		b[0]=(char)('A' + (ch >> 2)); b[1]=(ch & 3u)==3u ? 'N' : (char)('1' + (ch & 3u));
		(void)draw_text(cell_x[ch], y + 46u, b, 2u, 4u, 0u);
		/* A channel the order does not use: M in its last slot. */
		if (v->doc->orders[v->order][ch] == 0xFFu)
			(void)draw_text(cell_x[ch] + ((ch & 3u) == 3u ? 8u : 32u),
					y + 46u, "M", 1u, 7u, 0u);
	}
}

static void draw_rules(uint8_t page)
{
	uint16_t y = PAGE_Y(page), o; uint8_t chip;
	for (chip=0u; chip<5u; ++chip) {
		o=12u+(uint16_t)chip*124u; (void)gfx_line(o,y+44u,o,y+204u,2u,GFX_OP_COPY);
	}
	for (chip=0u; chip<4u; ++chip) {
		o=12u+(uint16_t)chip*124u;
		(void)gfx_line(o+37u,y+44u,o+37u,y+204u,2u,GFX_OP_COPY);
		(void)gfx_line(o+73u,y+44u,o+73u,y+204u,2u,GFX_OP_COPY);
		(void)gfx_line(o+110u,y+44u,o+110u,y+204u,2u,GFX_OP_COPY);
	}
}

static void draw_status(const struct view *v, uint8_t page)
{
	char b[64]; uint8_t n=0u; uint16_t y=PAGE_Y(page)+205u;
#define ADD(s) do { const char *p_=(s); while (*p_) b[n++]=*p_++; } while (0)
	(void)gfx_fill_rect(0u,y,512u,7u,1u,GFX_OP_COPY);
	ADD("OCT 4  STEP 1  INS 00  ROW ");
	b[n++]=hex_digit((uint8_t)v->first_row>>4); b[n++]=hex_digit((uint8_t)v->first_row); b[n++]='/';
	b[n++]=hex_digit((uint8_t)(v->doc->rows-1u)>>4); b[n++]=hex_digit((uint8_t)(v->doc->rows-1u));
	ADD("  FOCUS CHIP A  F1 HELP"); b[n]='\0';
#undef ADD
	(void)draw_text(0u,y,b,n,4u,1u);
}

int view_draw_page(struct view *v, uint8_t page, uint16_t first_row)
{
	struct ztrdoc_event row_events[ZTRDOC_CHANNELS]; uint8_t ch, row;
	if (v == 0 || page > 1u) return -1;
	active_view=v; v->first_row=first_row;
	for (ch=0u; ch<16u; ++ch)
		if (ztrdoc_decode_channel(v->order_buffer,ch,first_row,decoded[ch])<0) return -1;
	gfx_begin(); draw_title(v,page); draw_orders(v,page); draw_instruments(v,page);
	draw_song(v,page); draw_headers(v,page);
	for (row=0u; row<25u; ++row) {
		for (ch=0u; ch<16u; ++ch) row_events[ch]=decoded[ch][row];
		(void)view_draw_row(page,row,row_events);
	}
	draw_rules(page); draw_status(v,page);
	(void)gfx_fill_rect(0u,PAGE_Y(page)+55u,508u,6u,8u,GFX_OP_XOR);
	v->highlighted_row[page]=1u; v->highlighted_order[page]=1u;
	v->highlighted_instrument[page]=1u;
	return gfx_end();
}

int view_open(struct view *v, const struct ztrdoc *doc,
	      const uint8_t *order_buffer, uint16_t order)
{
	int result; uint8_t i;
	memset(v,0,sizeof(*v)); v->doc=doc; v->order_buffer=order_buffer; v->order=order;
	v->selected_instrument=0u; v->order_top=order >= 4u ? order : 0u;
	for (i=0u; i<doc->instrument_count; ++i) if (doc->instruments[i].number==0u) {
		if (i>=4u) v->instrument_top=(uint8_t)(i-3u); break;
	}
	active_view=v;
	result=gfx_open(); if (result<0) return result;
	result=gfx_set_mode(GFX_MODE_512x212_16); if (result<0) { gfx_close(); return result; }
	result=gfx_set_palette(0u,16u,sntrack_palette); if (result<0) { gfx_close(); return result; }
	return 0;
}

void view_close(void)
{
	active_view=0; gfx_close();
}
