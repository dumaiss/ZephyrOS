#include "ke_text.h"

uint32_t ke_visual_advance(uint32_t column, uint8_t byte)
{
	if (byte == 9u) return (column + 8ul) & ~7ul;
	return column + 1ul;
}

/* The text walks below read the store a page span at a time: one locate
 * per page, a byte loop inside it. */

ke_pos_t ke_line_start(struct ke_store *store, ke_pos_t position)
{
	if (position > ke_store_length(store)) position = ke_store_length(store);
	while (position != 0ul) {
		const uint8_t *page;
		uint16_t offset;
		uint16_t length;
		uint16_t i;

		if (ke_store_span(store, position - 1ul, &page, &offset,
				  &length) != KE_STORE_OK) break;
		/* page[offset] is the byte before `position`. */
		i = (uint16_t)(offset + 1u);
		do {
			--i;
			if (page[i] == 10u)
				return position - (ke_pos_t)(offset - i);
		} while (i != 0u);
		position -= (ke_pos_t)offset + 1ul;
	}
	return position;
}

ke_pos_t ke_line_end(struct ke_store *store, ke_pos_t position)
{
	ke_pos_t end = ke_store_length(store);

	while (position < end) {
		const uint8_t *page;
		uint16_t offset;
		uint16_t length;
		uint16_t i;

		if (ke_store_span(store, position, &page, &offset, &length) !=
		    KE_STORE_OK) break;
		for (i = offset; i < length; ++i)
			if (page[i] == 10u)
				return position + (ke_pos_t)(i - offset);
		position += (ke_pos_t)(length - offset);
	}
	return position;
}

ke_pos_t ke_walk_line(struct ke_store *store, ke_pos_t at, ke_pos_t stop,
		      uint32_t limit, uint32_t *column)
{
	ke_pos_t end = ke_store_length(store);
	uint32_t visual = 0ul;

	if (stop > end) stop = end;
	while (at < stop) {
		const uint8_t *page;
		uint16_t offset;
		uint16_t length;

		if (ke_store_span(store, at, &page, &offset, &length) !=
		    KE_STORE_OK) break;
		for (; offset < length && at < stop; ++offset, ++at) {
			uint8_t byte = page[offset];
			uint32_t next;

			if (byte == 10u) goto done;
			next = ke_visual_advance(visual, byte);
			if (next > limit) goto done;
			visual = next;
		}
	}
done:
	*column = visual;
	return at;
}

uint32_t ke_visual_column(struct ke_store *store, ke_pos_t position)
{
	uint32_t column;

	(void)ke_walk_line(store, ke_line_start(store, position), position,
			0xFFFFFFFFul, &column);
	return column;
}

ke_pos_t ke_position_for_column(struct ke_store *store, ke_pos_t line,
				uint32_t column)
{
	uint32_t reached;

	return ke_walk_line(store, line, ke_store_length(store), column, &reached);
}

static uint8_t fold_ascii(uint8_t byte)
{
	if (byte >= (uint8_t)'a' && byte <= (uint8_t)'z')
		return (uint8_t)(byte - ((uint8_t)'a' - (uint8_t)'A'));
	return byte;
}

int ke_find(struct ke_store *store, ke_pos_t from, const uint8_t *pattern,
	    uint16_t length, uint8_t match_case, ke_pos_t *found)
{
	ke_pos_t limit;
	ke_pos_t at;

	if (pattern == (const uint8_t *)0 || found == (ke_pos_t *)0 ||
	    length == 0u || from > ke_store_length(store) ||
	    (ke_pos_t)length > ke_store_length(store)) return 0;
	limit = ke_store_length(store) - length;
	for (at = from; at <= limit; ++at) {
		uint16_t i;
		uint8_t byte;

		for (i = 0u; i < length; ++i) {
			if (ke_store_byte(store, at + i, &byte) != KE_STORE_OK)
				return 0;
			if (match_case != 0u) {
				if (byte != pattern[i]) break;
			} else if (fold_ascii(byte) != fold_ascii(pattern[i])) {
				break;
			}
		}
		if (i == length) {
			*found = at;
			return 1;
		}
	}
	return 0;
}

int ke_normalize(struct ke_normalizer *state, const uint8_t *input,
		 uint16_t input_length, uint8_t *output, uint16_t output_size,
		 uint16_t *consumed, uint16_t *produced)
{
	uint16_t in = 0u;
	uint16_t out = 0u;

	if (state == (struct ke_normalizer *)0 || input == (const uint8_t *)0 ||
	    output == (uint8_t *)0 || consumed == (uint16_t *)0 ||
	    produced == (uint16_t *)0) return -1;
	while (in < input_length) {
		uint8_t byte = input[in];

		if (state->pending_cr != 0u) {
			if (out >= output_size) break;
			output[out++] = 10u;
			state->pending_cr = 0u;
			if (byte == 10u) {
				++in;
				continue;
			}
		}
		if (byte == 0x1au) {
			state->eof = 1u;
			++in;
			break;
		}
		if (byte == 0u) return -1;
		if (byte == 13u) {
			state->pending_cr = 1u;
			++in;
			continue;
		}
		if (out >= output_size) break;
		output[out++] = byte;
		++in;
	}
	*consumed = in;
	*produced = out;
	return 0;
}

int ke_normalize_finish(struct ke_normalizer *state, uint8_t *output,
			uint16_t output_size, uint16_t *produced)
{
	if (state == (struct ke_normalizer *)0 || output == (uint8_t *)0 ||
	    produced == (uint16_t *)0) return -1;
	*produced = 0u;
	if (state->pending_cr != 0u) {
		if (output_size == 0u) return -1;
		output[0] = 10u;
		*produced = 1u;
		state->pending_cr = 0u;
	}
	return 0;
}
