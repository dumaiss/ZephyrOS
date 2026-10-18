#include "sc2kos.h"

static struct sc_cell cells[SC_MAX_CELLS];
static unsigned int cell_count;
static char text_pool[SC_TEXT_POOL_SIZE];
static unsigned int text_used;
static unsigned char eval_state[SC_MAX_CELLS];

struct parser {
	const char *scan;
	unsigned char failed;
	unsigned char depth;
};

static unsigned char upper(unsigned char ch)
{
	if (ch >= 'a' && ch <= 'z') {
		return (unsigned char)(ch - ('a' - 'A'));
	}
	return ch;
}

static void skip_spaces(struct parser *p)
{
	while (*p->scan == ' ' || *p->scan == '\t') {
		++p->scan;
	}
}

/* The standalone KonaOS library deliberately carries division helpers but
 * no 32-bit multiply helper.  Keep the spreadsheet self-contained with the
 * usual shift/add implementation instead of pulling in a hosted runtime. */
static long multiply_long(long left, long right)
{
	unsigned long a;
	unsigned long b;
	unsigned long result = 0uL;
	unsigned char negative = (unsigned char)((left < 0L) != (right < 0L));

	a = left < 0L ? (unsigned long)(0uL - (unsigned long)left) :
		(unsigned long)left;
	b = right < 0L ? (unsigned long)(0uL - (unsigned long)right) :
		(unsigned long)right;
	while (b != 0uL) {
		if (b & 1uL) { result += a; }
		a <<= 1;
		b >>= 1;
	}
	return negative != 0u ? (long)(0uL - result) : (long)result;
}

void sheet_clear(void)
{
	cell_count = 0u;
	text_used = 0u;
}

unsigned int sheet_count(void)
{
	return cell_count;
}

struct sc_cell *sheet_cell_at(unsigned int index)
{
	if (index >= cell_count) {
		return (struct sc_cell *)0;
	}
	return &cells[index];
}

struct sc_cell *sheet_find(unsigned char col, unsigned char row)
{
	unsigned int low = 0u;
	unsigned int high = cell_count;

	while (low < high) {
		unsigned int middle = low + (high - low) / 2u;
		struct sc_cell *cell = &cells[middle];

		if (cell->row < row || (cell->row == row && cell->col < col)) {
			low = middle + 1u;
		} else {
			high = middle;
		}
	}
	if (low < cell_count && cells[low].row == row && cells[low].col == col) {
		return &cells[low];
	}
	return (struct sc_cell *)0;
}

const char *sheet_text(const struct sc_cell *cell)
{
	if (cell == (const struct sc_cell *)0 || cell->text_length == 0u) {
		return "";
	}
	return &text_pool[cell->text_offset];
}

static void compact_pool(void)
{
	unsigned int write_at = 0u;
	unsigned int done = 0u;

	for (;;) {
		unsigned int i;
		unsigned int best = SC_TEXT_POOL_SIZE;
		struct sc_cell *chosen = (struct sc_cell *)0;

		for (i = 0u; i < cell_count; ++i) {
			if (cells[i].text_length != 0u &&
			    cells[i].text_offset >= done &&
			    cells[i].text_offset < best) {
				best = cells[i].text_offset;
				chosen = &cells[i];
			}
		}
		if (chosen == (struct sc_cell *)0) {
			break;
		}
		memmove(&text_pool[write_at], &text_pool[chosen->text_offset],
			(unsigned int)chosen->text_length + 1u);
		done = chosen->text_offset + (unsigned int)chosen->text_length + 1u;
		chosen->text_offset = write_at;
		write_at += (unsigned int)chosen->text_length + 1u;
	}
	text_used = write_at;
}

static int store_text(struct sc_cell *cell, const char *text)
{
	unsigned int len = strlen(text);
	char previous[SC_INPUT_MAX + 1u];
	unsigned char previous_length = cell->text_length;

	if (len > SC_INPUT_MAX) {
		return SC_ERR_FULL;
	}
	/* Loading and replication normally append previously unseen cells.  The
	 * old implementation compacted the complete text pool for every such
	 * append, turning a large worksheet load into quadratic copying on the
	 * Z80.  Append directly while contiguous space remains. */
	if (previous_length == 0u && text_used + len + 1u <= SC_TEXT_POOL_SIZE) {
		cell->text_offset = text_used;
		cell->text_length = (unsigned char)len;
		memcpy(&text_pool[text_used], text, len + 1u);
		text_used += len + 1u;
		return SC_OK;
	}
	/* A shorter replacement fits in its existing allocation.  Leaving the
	 * unused tail as temporary fragmentation is far cheaper than moving the
	 * entire pool during interactive editing; a later space-pressure
	 * compaction reclaims it. */
	if (previous_length != 0u && len <= previous_length) {
		memcpy(&text_pool[cell->text_offset], text, len + 1u);
		cell->text_length = (unsigned char)len;
		return SC_OK;
	}
	if (previous_length != 0u) {
		memcpy(previous, &text_pool[cell->text_offset],
		       (unsigned int)previous_length + 1u);
	}
	cell->text_length = 0u;
	/* A growing replacement can also append without compaction.  Its former
	 * block becomes reclaimable fragmentation. */
	if (text_used + len + 1u <= SC_TEXT_POOL_SIZE) {
		cell->text_offset = text_used;
		cell->text_length = (unsigned char)len;
		memcpy(&text_pool[text_used], text, len + 1u);
		text_used += len + 1u;
		return SC_OK;
	}
	compact_pool();
	if (text_used + len + 1u > SC_TEXT_POOL_SIZE) {
		if (previous_length != 0u) {
			cell->text_offset = text_used;
			cell->text_length = previous_length;
			memcpy(&text_pool[text_used], previous,
			       (unsigned int)previous_length + 1u);
			text_used += (unsigned int)previous_length + 1u;
		}
		return SC_ERR_FULL;
	}
	cell->text_offset = text_used;
	cell->text_length = (unsigned char)len;
	memcpy(&text_pool[text_used], text, len + 1u);
	text_used += len + 1u;
	return SC_OK;
}

static struct sc_cell *make_cell(unsigned char col, unsigned char row)
{
	struct sc_cell *cell = sheet_find(col, row);
	unsigned int at;

	if (cell != (struct sc_cell *)0) {
		return cell;
	}
	if (cell_count >= SC_MAX_CELLS) {
		return (struct sc_cell *)0;
	}
	for (at = 0u; at < cell_count; ++at) {
		if (cells[at].row > row ||
		    (cells[at].row == row && cells[at].col > col)) {
			break;
		}
	}
	if (at < cell_count) {
		memmove(&cells[at + 1u], &cells[at],
			(cell_count - at) * sizeof(cells[0]));
	}
	++cell_count;
	cell = &cells[at];
	memset(cell, 0, sizeof(*cell));
	cell->col = col;
	cell->row = row;
	cell->precision = 2u;
	return cell;
}

static unsigned int cell_index(const struct sc_cell *cell)
{
	return (unsigned int)(cell - cells);
}

static int parse_ref_detail(const char *text, unsigned char *col,
			    unsigned char *row, const char **end,
			    unsigned char *col_absolute,
			    unsigned char *row_absolute)
{
	unsigned int column = 0u;
	unsigned int number = 0u;
	unsigned int letters = 0u;
	const char *scan = text;
	unsigned char fixed_col = 0u;
	unsigned char fixed_row = 0u;

	if (*scan == '$') { fixed_col = 1u; ++scan; }

	while (upper((unsigned char)*scan) >= 'A' &&
	       upper((unsigned char)*scan) <= 'Z' && letters < 2u) {
		column = column * 26u +
			(unsigned int)(upper((unsigned char)*scan) - 'A' + 1u);
		++scan;
		++letters;
	}
	if (letters == 0u || *scan < '0' || *scan > '9') {
		if (*scan != '$') { return 0; }
		fixed_row = 1u;
		++scan;
		if (*scan < '0' || *scan > '9') { return 0; }
	}
	while (*scan >= '0' && *scan <= '9') {
		number = number * 10u + (unsigned int)(*scan - '0');
		++scan;
		if (number > SC_MAX_ROWS) {
			return 0;
		}
	}
	if (column == 0u || column > SC_MAX_COLS || number == 0u) {
		return 0;
	}
	*col = (unsigned char)(column - 1u);
	*row = (unsigned char)(number - 1u);
	if (end != (const char **)0) {
		*end = scan;
	}
	if (col_absolute != (unsigned char *)0) { *col_absolute = fixed_col; }
	if (row_absolute != (unsigned char *)0) { *row_absolute = fixed_row; }
	return 1;
}

int sheet_parse_ref(const char *text, unsigned char *col, unsigned char *row,
		    const char **end)
{
	return parse_ref_detail(text, col, row, end,
				(unsigned char *)0, (unsigned char *)0);
}

int sheet_parse_range(const char *text, unsigned char *c1, unsigned char *r1,
		      unsigned char *c2, unsigned char *r2)
{
	const char *scan;
	unsigned char temp;

	while (*text == ' ' || *text == '\t') {
		++text;
	}
	if (!sheet_parse_ref(text, c1, r1, &scan)) {
		return 0;
	}
	*c2 = *c1;
	*r2 = *r1;
	if (*scan == ':') {
		++scan;
		if (!sheet_parse_ref(scan, c2, r2, &scan)) {
			return 0;
		}
	}
	while (*scan == ' ' || *scan == '\t') {
		++scan;
	}
	if (*scan != '\0') {
		return 0;
	}
	if (*c1 > *c2) {
		temp = *c1; *c1 = *c2; *c2 = temp;
	}
	if (*r1 > *r2) {
		temp = *r1; *r1 = *r2; *r2 = temp;
	}
	return 1;
}

void sheet_ref(char *dst, unsigned char col, unsigned char row)
{
	unsigned int number = (unsigned int)col + 1u;
	unsigned int at = 0u;
	char digits[6];

	if (number > 26u) {
		dst[at++] = number <= 52u ? 'A' : 'B';
	}
	if (number > 52u) { number -= 52u; }
	else if (number > 26u) { number -= 26u; }
	dst[at++] = (char)('A' + number - 1u);
	utoa(digits, (unsigned int)row + 1u, 10u, 0u, '0');
	strlcpy(&dst[at], digits, 6u);
}

static long parse_expression(struct parser *p);

static long scaled_multiply(long left, long right);

static long scaled_divide(struct parser *p, long left, long right)
{
	if (right == 0L) {
		p->failed = 1u;
		return 0L;
	}
	return multiply_long(left / right, SC_SCALE) +
		multiply_long(left % right, SC_SCALE) / right;
}

static long eval_cell(struct sc_cell *cell)
{
	unsigned int index = cell_index(cell);
	struct parser parser;
	long value;

	if (cell->type == SC_CELL_TEXT) {
		return 0L;
	}
	if (cell->type == SC_CELL_NUMBER) {
		return cell->value;
	}
	if (eval_state[index] == 2u) {
		return cell->value;
	}
	if (eval_state[index] == 1u) {
		cell->flags |= SC_FLAG_ERROR;
		return 0L;
	}
	eval_state[index] = 1u;
	parser.scan = sheet_text(cell);
	parser.failed = 0u;
	parser.depth = 0u;
	if (*parser.scan == '=') {
		++parser.scan;
	}
	value = parse_expression(&parser);
	skip_spaces(&parser);
	if (*parser.scan != '\0') {
		parser.failed = 1u;
	}
	if (parser.failed != 0u) {
		cell->flags |= SC_FLAG_ERROR;
		cell->value = 0L;
	} else {
		cell->flags &= (unsigned char)~SC_FLAG_ERROR;
		cell->value = value;
	}
	eval_state[index] = 2u;
	return cell->value;
}

static long range_function(struct parser *p, const char *name)
{
	unsigned char c1, r1, c2, r2;
	unsigned char col, row;
	const char *after;
	long result = 0L;
	unsigned int count = 0u;
	unsigned char have = 0u;

	skip_spaces(p);
	if (!sheet_parse_ref(p->scan, &c1, &r1, &after)) {
		p->failed = 1u;
		return 0L;
	}
	p->scan = after;
	c2 = c1;
	r2 = r1;
	if (*p->scan == ':') {
		++p->scan;
		if (!sheet_parse_ref(p->scan, &c2, &r2, &after)) {
			p->failed = 1u;
			return 0L;
		}
		p->scan = after;
	}
	if (c1 > c2) { col = c1; c1 = c2; c2 = col; }
	if (r1 > r2) { row = r1; r1 = r2; r2 = row; }
	skip_spaces(p);
	if (*p->scan != ')') {
		p->failed = 1u;
		return 0L;
	}
	++p->scan;
	for (row = r1; row <= r2; ++row) {
		for (col = c1; col <= c2; ++col) {
			struct sc_cell *cell = sheet_find(col, row);
			long value;

			if (cell == (struct sc_cell *)0 || cell->type == SC_CELL_TEXT) {
				continue;
			}
			value = eval_cell(cell);
			if (cell->flags & SC_FLAG_ERROR) {
				p->failed = 1u;
				return 0L;
			}
			if (strcmp(name, "COUNT") == 0) {
				++count;
				continue;
			}
			if (strcmp(name, "MIN") == 0 && (have == 0u || value < result)) {
				result = value;
			} else if (strcmp(name, "MAX") == 0 &&
				   (have == 0u || value > result)) {
				result = value;
			} else if (strcmp(name, "MIN") != 0 && strcmp(name, "MAX") != 0) {
				result += value;
			}
			have = 1u;
			++count;
		}
	}
	if (strcmp(name, "COUNT") == 0) {
		return multiply_long((long)count, SC_SCALE);
	}
	if (strcmp(name, "AVERAGE") == 0) {
		return count == 0u ? 0L : result / (long)count;
	}
	return result;
}

static long parse_number(struct parser *p)
{
	long whole = 0L;
	long fraction = 0L;
	unsigned char places = 0u;
	unsigned char any = 0u;

	while (*p->scan >= '0' && *p->scan <= '9') {
		whole = multiply_long(whole, 10L) + (long)(*p->scan - '0');
		++p->scan;
		any = 1u;
	}
	if (*p->scan == '.') {
		++p->scan;
		while (*p->scan >= '0' && *p->scan <= '9') {
			if (places < 3u) {
				fraction = multiply_long(fraction, 10L) +
					(long)(*p->scan - '0');
				++places;
			}
			++p->scan;
			any = 1u;
		}
	}
	if (any == 0u) {
		p->failed = 1u;
		return 0L;
	}
	while (places < 3u) {
		fraction = multiply_long(fraction, 10L);
		++places;
	}
	return multiply_long(whole, SC_SCALE) + fraction;
}

static unsigned char function_separator(struct parser *p)
{
	skip_spaces(p);
	if (*p->scan != ',') {
		p->failed = 1u;
		return 0u;
	}
	++p->scan;
	return 1u;
}

static unsigned char function_end(struct parser *p)
{
	skip_spaces(p);
	if (*p->scan != ')') {
		p->failed = 1u;
		return 0u;
	}
	++p->scan;
	return 1u;
}

static long scaled_power(struct parser *p, long base, long exponent_value)
{
	long result = SC_SCALE;
	long exponent;
	unsigned char negative = 0u;

	if (exponent_value % SC_SCALE != 0L) {
		p->failed = 1u;
		return 0L;
	}
	exponent = exponent_value / SC_SCALE;
	if (exponent < 0L) {
		negative = 1u;
		exponent = -exponent;
	}
	if (exponent > 600L) {
		p->failed = 1u;
		return 0L;
	}
	while (exponent != 0L) {
		if (exponent & 1L) { result = scaled_multiply(result, base); }
		exponent >>= 1;
		if (exponent != 0L) { base = scaled_multiply(base, base); }
	}
	if (negative != 0u) { result = scaled_divide(p, SC_SCALE, result); }
	return result;
}

static unsigned char leap_year(unsigned int year)
{
	return (unsigned char)((year % 4u == 0u && year % 100u != 0u) ||
			       year % 400u == 0u);
}

static unsigned char month_days(unsigned int year, unsigned int month)
{
	static const unsigned char days[12] = {
		31u, 28u, 31u, 30u, 31u, 30u,
		31u, 31u, 30u, 31u, 30u, 31u
	};

	if (month == 2u && leap_year(year) != 0u) { return 29u; }
	return days[month - 1u];
}

static long make_date(struct parser *p, long year_value, long month_value,
		      long day_value)
{
	unsigned int year;
	unsigned int month;
	unsigned int day;
	unsigned int scan;
	long serial = 1L;

	if (year_value % SC_SCALE != 0L || month_value % SC_SCALE != 0L ||
	    day_value % SC_SCALE != 0L) {
		p->failed = 1u;
		return 0L;
	}
	year = (unsigned int)(year_value / SC_SCALE);
	month = (unsigned int)(month_value / SC_SCALE);
	day = (unsigned int)(day_value / SC_SCALE);
	if (year < 1900u || year > 2099u || month < 1u || month > 12u ||
	    day < 1u || day > month_days(year, month)) {
		p->failed = 1u;
		return 0L;
	}
	for (scan = 1900u; scan < year; ++scan) {
		serial += leap_year(scan) != 0u ? 366L : 365L;
	}
	for (scan = 1u; scan < month; ++scan) {
		serial += (long)month_days(year, scan);
	}
	serial += (long)day - 1L;
	return multiply_long(serial, SC_SCALE);
}

static long date_part(struct parser *p, long serial_value, unsigned char part)
{
	long serial;
	unsigned int year = 1900u;
	unsigned int month = 1u;

	if (serial_value % SC_SCALE != 0L || serial_value < SC_SCALE) {
		p->failed = 1u;
		return 0L;
	}
	serial = serial_value / SC_SCALE - 1L;
	while (serial >= (leap_year(year) != 0u ? 366L : 365L) && year < 2100u) {
		serial -= leap_year(year) != 0u ? 366L : 365L;
		++year;
	}
	while (month <= 12u && serial >= (long)month_days(year, month)) {
		serial -= (long)month_days(year, month);
		++month;
	}
	if (year > 2099u || month > 12u) {
		p->failed = 1u;
		return 0L;
	}
	if (part == 0u) { return multiply_long((long)year, SC_SCALE); }
	if (part == 1u) { return multiply_long((long)month, SC_SCALE); }
	return multiply_long(serial + 1L, SC_SCALE);
}

static long round_scaled(long value, long unit)
{
	unsigned long magnitude;
	unsigned char negative = value < 0L ? 1u : 0u;

	magnitude = negative != 0u ? (unsigned long)(0uL - (unsigned long)value) :
		(unsigned long)value;
	magnitude += (unsigned long)unit / 2uL;
	magnitude /= (unsigned long)unit;
	magnitude = (unsigned long)multiply_long((long)magnitude, unit);
	return negative != 0u ? (long)(0uL - magnitude) : (long)magnitude;
}

static long square_root(struct parser *p, long value)
{
	long guess;
	unsigned char i;

	if (value < 0L) { p->failed = 1u; return 0L; }
	if (value == 0L) { return 0L; }
	guess = value > SC_SCALE ? value : SC_SCALE;
	for (i = 0u; i < 16u; ++i) {
		long next = (guess + scaled_divide(p, value, guess)) / 2L;

		if (next == guess || next == guess - 1L) { return next; }
		guess = next;
	}
	return guess;
}

static long scalar_function(struct parser *p, const char *name)
{
	long a = parse_expression(p);
	long b = 0L;
	long c = 0L;
	long d = 0L;

	if (strcmp(name, "ABS") == 0 || strcmp(name, "INT") == 0 ||
	    strcmp(name, "SIGN") == 0 || strcmp(name, "NOT") == 0 ||
	    strcmp(name, "SQRT") == 0 ||
	    strcmp(name, "YEAR") == 0 || strcmp(name, "MONTH") == 0 ||
	    strcmp(name, "DAY") == 0) {
		if (!function_end(p)) { return 0L; }
		if (strcmp(name, "ABS") == 0) { return a < 0L ? -a : a; }
		if (strcmp(name, "INT") == 0) {
			return multiply_long(a / SC_SCALE, SC_SCALE);
		}
		if (strcmp(name, "SIGN") == 0) {
			return a < 0L ? -SC_SCALE : (a > 0L ? SC_SCALE : 0L);
		}
		if (strcmp(name, "NOT") == 0) { return a == 0L ? SC_SCALE : 0L; }
		if (strcmp(name, "SQRT") == 0) { return square_root(p, a); }
		if (strcmp(name, "YEAR") == 0) { return date_part(p, a, 0u); }
		if (strcmp(name, "MONTH") == 0) { return date_part(p, a, 1u); }
		return date_part(p, a, 2u);
	}
	if (!function_separator(p)) { return 0L; }
	b = parse_expression(p);
	if (strcmp(name, "MOD") == 0 || strcmp(name, "POWER") == 0 ||
	    strcmp(name, "ROUND") == 0 || strcmp(name, "AND") == 0 ||
	    strcmp(name, "OR") == 0) {
		long unit;

		if (!function_end(p)) { return 0L; }
		if (strcmp(name, "MOD") == 0) {
			if (b == 0L) { p->failed = 1u; return 0L; }
			return a % b;
		}
		if (strcmp(name, "POWER") == 0) { return scaled_power(p, a, b); }
		if (strcmp(name, "AND") == 0) {
			return a != 0L && b != 0L ? SC_SCALE : 0L;
		}
		if (strcmp(name, "OR") == 0) {
			return a != 0L || b != 0L ? SC_SCALE : 0L;
		}
		if (b % SC_SCALE != 0L || b < 0L || b > 3L * SC_SCALE) {
			p->failed = 1u;
			return 0L;
		}
		unit = b == 0L ? 1000L : (b == 1000L ? 100L : (b == 2000L ? 10L : 1L));
		return round_scaled(a, unit);
	}
	if (!function_separator(p)) { return 0L; }
	c = parse_expression(p);
	if (strcmp(name, "IF") == 0 || strcmp(name, "DATE") == 0 ||
	    strcmp(name, "PMT") == 0) {
		long factor;

		if (!function_end(p)) { return 0L; }
		if (strcmp(name, "IF") == 0) { return a != 0L ? b : c; }
		if (strcmp(name, "DATE") == 0) { return make_date(p, a, b, c); }
		if (a == 0L) {
			if (b <= 0L) { p->failed = 1u; return 0L; }
			return -scaled_divide(p, c, b);
		}
		factor = scaled_power(p, SC_SCALE + a, b);
		if (p->failed != 0u || factor == SC_SCALE) {
			p->failed = 1u;
			return 0L;
		}
		return -scaled_divide(p, scaled_multiply(scaled_multiply(a, c), factor),
				      factor - SC_SCALE);
	}
	if (!function_separator(p)) { return 0L; }
	d = parse_expression(p);
	if (!function_end(p)) { return 0L; }
	if (strcmp(name, "FV") == 0) {
		long factor = scaled_power(p, SC_SCALE + a, b);
		long annuity;

		if (a == 0L) {
			annuity = multiply_long(c, b / SC_SCALE);
		} else {
			annuity = scaled_divide(p, scaled_multiply(c, factor - SC_SCALE), a);
		}
		return -(scaled_multiply(d, factor) + annuity);
	}
	p->failed = 1u;
	return 0L;
}

static long parse_factor(struct parser *p)
{
	long value;
	unsigned char col, row;
	const char *after;

	skip_spaces(p);
	if (++p->depth > 24u) {
		p->failed = 1u;
		return 0L;
	}
	if (*p->scan == '+') {
		++p->scan;
		value = parse_factor(p);
		--p->depth;
		return value;
	}
	if (*p->scan == '-') {
		++p->scan;
		value = -parse_factor(p);
		--p->depth;
		return value;
	}
	if (*p->scan == '(') {
		++p->scan;
		value = parse_expression(p);
		skip_spaces(p);
		if (*p->scan != ')') {
			p->failed = 1u;
		} else {
			++p->scan;
		}
		--p->depth;
		return value;
	}
	if ((*p->scan >= '0' && *p->scan <= '9') || *p->scan == '.') {
		value = parse_number(p);
		--p->depth;
		return value;
	}
	if (sheet_parse_ref(p->scan, &col, &row, &after)) {
		struct sc_cell *cell;

		p->scan = after;
		cell = sheet_find(col, row);
		value = cell == (struct sc_cell *)0 ? 0L : eval_cell(cell);
		if (cell != (struct sc_cell *)0 && (cell->flags & SC_FLAG_ERROR)) {
			p->failed = 1u;
		}
		--p->depth;
		return value;
	}
	if (upper((unsigned char)*p->scan) >= 'A' &&
	    upper((unsigned char)*p->scan) <= 'Z') {
		char name[9];
		unsigned char n = 0u;

		while (upper((unsigned char)*p->scan) >= 'A' &&
		       upper((unsigned char)*p->scan) <= 'Z' && n < 8u) {
			name[n++] = (char)upper((unsigned char)*p->scan++);
		}
		name[n] = '\0';
		skip_spaces(p);
		if (*p->scan == '(' &&
		    (strcmp(name, "SUM") == 0 || strcmp(name, "AVERAGE") == 0 ||
		     strcmp(name, "MIN") == 0 || strcmp(name, "MAX") == 0 ||
		     strcmp(name, "COUNT") == 0)) {
			++p->scan;
			value = range_function(p, name);
			--p->depth;
			return value;
		}
		if (*p->scan == '(' &&
		    (strcmp(name, "ABS") == 0 || strcmp(name, "INT") == 0 ||
		     strcmp(name, "SIGN") == 0 || strcmp(name, "ROUND") == 0 ||
		     strcmp(name, "SQRT") == 0 ||
		     strcmp(name, "MOD") == 0 || strcmp(name, "POWER") == 0 ||
		     strcmp(name, "IF") == 0 || strcmp(name, "AND") == 0 ||
		     strcmp(name, "OR") == 0 || strcmp(name, "NOT") == 0 ||
		     strcmp(name, "DATE") == 0 || strcmp(name, "YEAR") == 0 ||
		     strcmp(name, "MONTH") == 0 || strcmp(name, "DAY") == 0 ||
		     strcmp(name, "PMT") == 0 || strcmp(name, "FV") == 0)) {
			++p->scan;
			value = scalar_function(p, name);
			--p->depth;
			return value;
		}
	}
	p->failed = 1u;
	--p->depth;
	return 0L;
}

static long scaled_multiply(long left, long right)
{
	long whole = left / SC_SCALE;
	long fraction = left % SC_SCALE;

	return multiply_long(whole, right) +
		multiply_long(fraction, right) / SC_SCALE;
}

static long parse_term(struct parser *p)
{
	long value = parse_factor(p);

	for (;;) {
		long right;
		unsigned char op;

		skip_spaces(p);
		op = (unsigned char)*p->scan;
		if (op != '*' && op != '/') {
			break;
		}
		++p->scan;
		right = parse_factor(p);
		if (op == '*') {
			value = scaled_multiply(value, right);
		} else {
			value = scaled_divide(p, value, right);
		}
	}
	return value;
}

static long parse_additive(struct parser *p)
{
	long value = parse_term(p);

	for (;;) {
		long right;
		unsigned char op;

		skip_spaces(p);
		op = (unsigned char)*p->scan;
		if (op != '+' && op != '-') {
			break;
		}
		++p->scan;
		right = parse_term(p);
		if (op == '+') {
			value += right;
		} else {
			value -= right;
		}
	}
	return value;
}

static long parse_expression(struct parser *p)
{
	long value = parse_additive(p);

	skip_spaces(p);
	if (*p->scan == '=' || *p->scan == '<' || *p->scan == '>') {
		unsigned char first = (unsigned char)*p->scan++;
		unsigned char second = 0u;
		long right;

		if ((first == '<' && (*p->scan == '=' || *p->scan == '>')) ||
		    (first == '>' && *p->scan == '=')) {
			second = (unsigned char)*p->scan++;
		}
		right = parse_additive(p);
		if (first == '=') { value = value == right ? SC_SCALE : 0L; }
		else if (first == '<' && second == '>') {
			value = value != right ? SC_SCALE : 0L;
		} else if (first == '<' && second == '=') {
			value = value <= right ? SC_SCALE : 0L;
		} else if (first == '>' && second == '=') {
			value = value >= right ? SC_SCALE : 0L;
		} else if (first == '<') { value = value < right ? SC_SCALE : 0L; }
		else { value = value > right ? SC_SCALE : 0L; }
	}
	return value;
}

void sheet_recalculate(void)
{
	unsigned int i;

	memset(eval_state, 0, sizeof(eval_state));
	for (i = 0u; i < cell_count; ++i) {
		if (cells[i].type == SC_CELL_FORMULA) {
			(void)eval_cell(&cells[i]);
		}
	}
}

static unsigned char looks_formula(const char *text)
{
	unsigned char col, row;
	const char *end;
	const char *scan = text;

	while (*scan == ' ' || *scan == '\t') { ++scan; }
	if (*scan == '=' || *scan == '+' || *scan == '-' || *scan == '(' ||
	    *scan == '.' || (*scan >= '0' && *scan <= '9')) {
		return 1u;
	}
	if (sheet_parse_ref(scan, &col, &row, &end)) {
		return 1u;
	}
	while (*scan != '\0') {
		if (*scan == '(') {
			return 1u;
		}
		++scan;
	}
	return 0u;
}

int sheet_import(unsigned char col, unsigned char row, unsigned char type,
		 unsigned char flags, unsigned char precision, const char *text)
{
	struct sc_cell *cell;
	int result;

	if (col >= SC_MAX_COLS || row >= SC_MAX_ROWS ||
	    type < SC_CELL_TEXT || type > SC_CELL_FORMULA) {
		return SC_ERR_RANGE;
	}
	cell = make_cell(col, row);
	if (cell == (struct sc_cell *)0) {
		return SC_ERR_FULL;
	}
	result = store_text(cell, text);
	if (result < 0) {
		return result;
	}
	cell->type = type;
	cell->flags = (unsigned char)(flags & SC_FLAG_PROTECTED);
	cell->precision = precision > 3u ? 3u : precision;
	return SC_OK;
}

int sheet_set(unsigned char col, unsigned char row, const char *text)
{
	struct sc_cell *cell = sheet_find(col, row);
	unsigned char type;
	int result;

	if (cell != (struct sc_cell *)0 && (cell->flags & SC_FLAG_PROTECTED)) {
		return SC_ERR_PROTECTED;
	}
	if (*text == '\0') {
		return sheet_blank(col, row, col, row);
	}
	if (*text == '\'' || *text == '"') {
		type = SC_CELL_TEXT;
		++text;
	} else {
		type = looks_formula(text) ? SC_CELL_FORMULA : SC_CELL_TEXT;
	}
	cell = make_cell(col, row);
	if (cell == (struct sc_cell *)0) {
		return SC_ERR_FULL;
	}
	result = store_text(cell, text);
	if (result < 0) {
		return result;
	}
	cell->type = type;
	cell->flags &= SC_FLAG_PROTECTED;
	sheet_recalculate();
	if (cell->flags & SC_FLAG_ERROR) {
		return SC_ERR_FORMULA;
	}
	return SC_OK;
}

static void remove_index(unsigned int index)
{
	if (index + 1u < cell_count) {
		memmove(&cells[index], &cells[index + 1u],
			(cell_count - index - 1u) * sizeof(cells[0]));
	}
	--cell_count;
}

int sheet_blank(unsigned char c1, unsigned char r1,
		unsigned char c2, unsigned char r2)
{
	unsigned int i = 0u;

	for (i = 0u; i < cell_count; ++i) {
		if (cells[i].col >= c1 && cells[i].col <= c2 &&
		    cells[i].row >= r1 && cells[i].row <= r2 &&
		    (cells[i].flags & SC_FLAG_PROTECTED)) {
			return SC_ERR_PROTECTED;
		}
	}
	i = 0u;
	while (i < cell_count) {
		if (cells[i].col >= c1 && cells[i].col <= c2 &&
		    cells[i].row >= r1 && cells[i].row <= r2) {
			remove_index(i);
		} else {
			++i;
		}
	}
	sheet_recalculate();
	return SC_OK;
}

static int translate_formula(const char *source, int col_delta, int row_delta,
			     char *target, unsigned int size)
{
	unsigned int out = 0u;

	while (*source != '\0') {
		unsigned char col, row;
		unsigned char fixed_col, fixed_row;
		const char *after;

		if (parse_ref_detail(source, &col, &row, &after,
				     &fixed_col, &fixed_row)) {
			int new_col = (int)col + (fixed_col != 0u ? 0 : col_delta);
			int new_row = (int)row + (fixed_row != 0u ? 0 : row_delta);
			char ref[8];
			unsigned int length;

			if (new_col < 0 || new_col >= (int)SC_MAX_COLS ||
			    new_row < 0 || new_row >= (int)SC_MAX_ROWS) {
				return SC_ERR_RANGE;
			}
			sheet_ref(ref, (unsigned char)new_col, (unsigned char)new_row);
			length = strlen(ref) + (unsigned int)fixed_col +
				(unsigned int)fixed_row;
			if (out + length >= size) { return SC_ERR_FULL; }
			if (fixed_col != 0u) { target[out++] = '$'; }
			{
				unsigned int letters = 0u;

				while (ref[letters] >= 'A' && ref[letters] <= 'Z') {
					target[out++] = ref[letters++];
				}
				if (fixed_row != 0u) { target[out++] = '$'; }
				strlcpy(&target[out], &ref[letters], size - out);
				out += strlen(&ref[letters]);
			}
			source = after;
		} else {
			if (out + 1u >= size) { return SC_ERR_FULL; }
			target[out++] = *source++;
		}
	}
	target[out] = '\0';
	return SC_OK;
}

int sheet_copy(unsigned char src_col, unsigned char src_row,
	       unsigned char dst_col, unsigned char dst_row, unsigned char move)
{
	struct sc_cell *src = sheet_find(src_col, src_row);
	struct sc_cell *dst = sheet_find(dst_col, dst_row);
	char text[SC_INPUT_MAX + 1u];
	unsigned char type, flags, precision;
	int result;

	if (dst != (struct sc_cell *)0 && (dst->flags & SC_FLAG_PROTECTED)) {
		return SC_ERR_PROTECTED;
	}
	if (move != 0u && src != (struct sc_cell *)0 &&
	    (src->flags & SC_FLAG_PROTECTED)) {
		return SC_ERR_PROTECTED;
	}
	if (src == (struct sc_cell *)0) {
		return sheet_blank(dst_col, dst_row, dst_col, dst_row);
	}
	strlcpy(text, sheet_text(src), sizeof(text));
	type = src->type;
	flags = (unsigned char)(src->flags & SC_FLAG_PROTECTED);
	precision = src->precision;
	if (move == 0u && type == SC_CELL_FORMULA) {
		result = translate_formula(sheet_text(src),
			(int)dst_col - (int)src_col, (int)dst_row - (int)src_row,
			text, sizeof(text));
		if (result != SC_OK) { return result; }
	}
	result = sheet_import(dst_col, dst_row, type, flags, precision, text);
	if (result == SC_OK && move != 0u &&
	    (src_col != dst_col || src_row != dst_row)) {
		result = sheet_blank(src_col, src_row, src_col, src_row);
	}
	sheet_recalculate();
	return result;
}

int sheet_replicate(unsigned char src_col, unsigned char src_row,
		    unsigned char c1, unsigned char r1,
		    unsigned char c2, unsigned char r2)
{
	struct sc_cell *src = sheet_find(src_col, src_row);
	char source[SC_INPUT_MAX + 1u];
	unsigned char type = 0u;
	unsigned char flags = 0u;
	unsigned char precision = 0u;
	unsigned int col, row;
	int result = SC_OK;

	for (row = r1; row <= r2; ++row) {
		for (col = c1; col <= c2; ++col) {
			struct sc_cell *dst = sheet_find((unsigned char)col,
						      (unsigned char)row);
			if (dst != (struct sc_cell *)0 &&
			    (dst->flags & SC_FLAG_PROTECTED) != 0u &&
			    (col != src_col || row != src_row)) {
				return SC_ERR_PROTECTED;
			}
		}
	}
	if (src == (struct sc_cell *)0) {
		return sheet_blank(c1, r1, c2, r2);
	}
	strlcpy(source, sheet_text(src), sizeof(source));
	type = src->type;
	flags = (unsigned char)(src->flags & SC_FLAG_PROTECTED);
	precision = src->precision;
	for (row = r1; result == SC_OK && row <= r2; ++row) {
		for (col = c1; result == SC_OK && col <= c2; ++col) {
			char text[SC_INPUT_MAX + 1u];

			strlcpy(text, source, sizeof(text));
			if (type == SC_CELL_FORMULA) {
				result = translate_formula(source,
					(int)col - (int)src_col,
					(int)row - (int)src_row,
					text, sizeof(text));
			}
			if (result == SC_OK) {
				result = sheet_import((unsigned char)col, (unsigned char)row,
						      type, flags, precision, text);
			}
		}
	}
	sheet_recalculate();
	return result;
}

int sheet_insert_row(unsigned char row)
{
	unsigned int i;

	for (i = 0u; i < cell_count; ++i) {
		if (cells[i].row == SC_MAX_ROWS - 1u) {
			return SC_ERR_RANGE;
		}
	}
	for (i = 0u; i < cell_count; ++i) {
		if (cells[i].row >= row) { ++cells[i].row; }
	}
	sheet_recalculate();
	return SC_OK;
}

int sheet_insert_col(unsigned char col)
{
	unsigned int i;

	for (i = 0u; i < cell_count; ++i) {
		if (cells[i].col == SC_MAX_COLS - 1u) {
			return SC_ERR_RANGE;
		}
	}
	for (i = 0u; i < cell_count; ++i) {
		if (cells[i].col >= col) { ++cells[i].col; }
	}
	sheet_recalculate();
	return SC_OK;
}

int sheet_delete_row(unsigned char row)
{
	unsigned int i = 0u;

	for (i = 0u; i < cell_count; ++i) {
		if (cells[i].row == row && (cells[i].flags & SC_FLAG_PROTECTED)) {
			return SC_ERR_PROTECTED;
		}
	}
	i = 0u;
	while (i < cell_count) {
		if (cells[i].row == row) {
			remove_index(i);
		} else {
			if (cells[i].row > row) { --cells[i].row; }
			++i;
		}
	}
	sheet_recalculate();
	return SC_OK;
}

int sheet_delete_col(unsigned char col)
{
	unsigned int i = 0u;

	for (i = 0u; i < cell_count; ++i) {
		if (cells[i].col == col && (cells[i].flags & SC_FLAG_PROTECTED)) {
			return SC_ERR_PROTECTED;
		}
	}
	i = 0u;
	while (i < cell_count) {
		if (cells[i].col == col) {
			remove_index(i);
		} else {
			if (cells[i].col > col) { --cells[i].col; }
			++i;
		}
	}
	sheet_recalculate();
	return SC_OK;
}

void sheet_used(unsigned char *last_col, unsigned char *last_row)
{
	unsigned int i;

	*last_col = 0u;
	*last_row = 0u;
	for (i = 0u; i < cell_count; ++i) {
		if (cells[i].col > *last_col) { *last_col = cells[i].col; }
		if (cells[i].row > *last_row) { *last_row = cells[i].row; }
	}
}

void sheet_format_value(char *dst, unsigned int size, long value,
			unsigned char precision)
{
	unsigned long magnitude;
	unsigned long whole;
	unsigned int fraction;
	char left[16];
	char right[5];
	unsigned int at = 0u;

	if (size == 0u) { return; }
	if (value < 0L) {
		dst[at++] = '-';
		magnitude = (unsigned long)(-value);
	} else {
		magnitude = (unsigned long)value;
	}
	whole = magnitude / (unsigned long)SC_SCALE;
	fraction = (unsigned int)(magnitude % (unsigned long)SC_SCALE);
	ultoa(left, whole, 10u, 0u, '0');
	strlcpy(&dst[at], left, size > at ? size - at : 0u);
	at = strlen(dst);
	if (precision > 3u) { precision = 3u; }
	if (precision != 0u && at + 1u < size) {
		dst[at++] = '.';
		dst[at] = '\0';
		utoa(right, fraction, 10u, 3u, '0');
		right[precision] = '\0';
		strlcpy(&dst[at], right, size - at);
		while (precision != 0u) {
			unsigned int len = strlen(dst);
			if (len == 0u || dst[len - 1u] != '0') { break; }
			dst[len - 1u] = '\0';
			--precision;
		}
		at = strlen(dst);
		if (at != 0u && dst[at - 1u] == '.') { dst[at - 1u] = '\0'; }
	}
}
