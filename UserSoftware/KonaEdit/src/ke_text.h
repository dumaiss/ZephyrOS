#ifndef KONAEDIT_TEXT_H
#define KONAEDIT_TEXT_H

#include "ke_store.h"

struct ke_normalizer {
	uint8_t pending_cr;
	uint8_t eof;
};

uint32_t ke_visual_advance(uint32_t column, uint8_t byte);
ke_pos_t ke_line_start(struct ke_store *store, ke_pos_t position);
ke_pos_t ke_line_end(struct ke_store *store, ke_pos_t position);
/* Walk a line from `at`: stop at `stop`, at the newline or the document's
 * end, or before a byte that would pass visual column `limit`.  The
 * position reached; *column is its visual column. */
ke_pos_t ke_walk_line(struct ke_store *store, ke_pos_t at, ke_pos_t stop,
		      uint32_t limit, uint32_t *column);
uint32_t ke_visual_column(struct ke_store *store, ke_pos_t position);
ke_pos_t ke_position_for_column(struct ke_store *store, ke_pos_t line,
				uint32_t column);
int ke_find(struct ke_store *store, ke_pos_t from, const uint8_t *pattern,
	    uint16_t length, uint8_t match_case, ke_pos_t *found);
int ke_normalize(struct ke_normalizer *state, const uint8_t *input,
		 uint16_t input_length, uint8_t *output, uint16_t output_size,
		 uint16_t *consumed, uint16_t *produced);
int ke_normalize_finish(struct ke_normalizer *state, uint8_t *output,
			uint16_t output_size, uint16_t *produced);

#endif
