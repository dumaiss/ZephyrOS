/* Private helpers shared by the procedural widget archive.
 *
 * Widgets use only the public tui.h surface.  This header carries no cell
 * map, no terminal knowledge and no core-private declaration.
 */

#ifndef KONA_WIDGET_INTERNAL_H
#define KONA_WIDGET_INTERNAL_H

#include "tui_widgets.h"

uint16_t w_strlen(const char *text);

/* Clip a widget rectangle to the active window.  out->w/h are zero when
 * the rectangle is empty or entirely outside. */
int w_clip(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
	   struct tui_rect *out);

/* Draw at most max characters with one semantic style, one cell at a
 * time, so no global style is changed.  Returns the characters drawn. */
uint8_t w_text(uint8_t x, uint8_t y, const char *text, uint8_t max,
	       tui_style_t style);

void w_blank(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
	     tui_style_t style);

int w_button_mask_valid(uint16_t mask);

#endif /* KONA_WIDGET_INTERNAL_H */
