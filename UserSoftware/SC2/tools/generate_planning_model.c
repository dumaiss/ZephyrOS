#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sc2kos.h"

#define FIRST_MONTH_COL 1u
#define LAST_MONTH_COL  12u
#define TOTAL_COL       13u
#define AVERAGE_COL     14u

static void fail_cell(unsigned char col, unsigned char row, const char *text,
		      int result)
{
	char name[8];

	sheet_ref(name, col, row);
	fprintf(stderr, "generator: %s rejected (%d): %s\n", name, result, text);
	exit(1);
}

static void put_cell(unsigned char col, unsigned char row, unsigned char type,
		     unsigned char precision, const char *text)
{
	int result = sheet_import(col, row, type, 0u, precision, text);

	if (result != SC_OK) { fail_cell(col, row, text, result); }
}

static void put_label(unsigned char row, const char *text)
{
	put_cell(0u, row, SC_CELL_TEXT, 0u, text);
}

static void put_value(unsigned char col, unsigned char row, const char *text,
		      unsigned char precision)
{
	put_cell(col, row, SC_CELL_FORMULA, precision, text);
}

static void cell_name(char *name, unsigned char col, unsigned char row)
{
	sheet_ref(name, col, row);
}

static void put_binary(unsigned char col, unsigned char row,
		       unsigned char left_row, char op, unsigned char right_row,
		       unsigned char precision)
{
	char left[8];
	char right[8];
	char formula[32];

	cell_name(left, col, left_row);
	cell_name(right, col, right_row);
	snprintf(formula, sizeof(formula), "=%s%c%s", left, op, right);
	put_value(col, row, formula, precision);
}

static void put_total(unsigned char row)
{
	char first[8];
	char last[8];
	char formula[32];

	cell_name(first, FIRST_MONTH_COL, row);
	cell_name(last, LAST_MONTH_COL, row);
	snprintf(formula, sizeof(formula), "=SUM(%s:%s)", first, last);
	put_value(TOTAL_COL, row, formula, 2u);
	snprintf(formula, sizeof(formula), "=AVERAGE(%s:%s)", first, last);
	put_value(AVERAGE_COL, row, formula, 2u);
}

static void put_average(unsigned char row, unsigned char precision)
{
	char first[8];
	char last[8];
	char formula[32];

	cell_name(first, FIRST_MONTH_COL, row);
	cell_name(last, LAST_MONTH_COL, row);
	snprintf(formula, sizeof(formula), "=AVERAGE(%s:%s)", first, last);
	put_value(TOTAL_COL, row, formula, precision);
}

static void put_ending(unsigned char row, unsigned char precision)
{
	char last[8];
	char formula[16];

	cell_name(last, LAST_MONTH_COL, row);
	snprintf(formula, sizeof(formula), "=%s", last);
	put_value(TOTAL_COL, row, formula, precision);
}

int main(int argc, char **argv)
{
	static const char *const months[12] = {
		"Jan", "Feb", "Mar", "Apr", "May", "Jun",
		"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	};
	static const unsigned char total_rows[] = {
		3u, 5u, 6u, 7u, 9u, 10u, 11u, 12u,
		15u, 16u, 17u, 18u, 19u, 20u, 21u,
		24u, 25u, 26u, 27u, 28u, 36u, 37u, 50u
	};
	static const unsigned char average_rows[] = {
		4u, 8u, 13u, 22u, 23u, 34u, 41u, 42u, 43u, 44u, 45u, 46u, 47u
	};
	const char *path = argc > 1 ? argv[1] : "examples/planning_model.ksc";
	unsigned int month;
	unsigned int i;
	unsigned int saved_count;
	char text[64];

	sheet_clear();
	put_label(0u, "SC2KOS 12-MONTH BUSINESS PLAN");
	put_label(1u, "Month");
	for (month = 0u; month < 12u; ++month) {
		put_cell((unsigned char)(FIRST_MONTH_COL + month), 1u,
			 SC_CELL_TEXT, 0u, months[month]);
	}
	put_cell(TOTAL_COL, 1u, SC_CELL_TEXT, 0u, "Annual/End");
	put_cell(AVERAGE_COL, 1u, SC_CELL_TEXT, 0u, "Monthly Avg");

	put_label(2u, "OPERATING PLAN");
	put_label(3u, "Units sold");
	put_label(4u, "Unit price");
	put_label(5u, "Product sales");
	put_label(6u, "Service revenue");
	put_label(7u, "Total revenue");
	put_label(8u, "Unit cost");
	put_label(9u, "Product costs");
	put_label(10u, "Service costs");
	put_label(11u, "Total COGS");
	put_label(12u, "Gross profit");
	put_label(13u, "Gross margin");
	put_label(14u, "OPERATING EXPENSES");
	put_label(15u, "Payroll");
	put_label(16u, "Rent");
	put_label(17u, "Marketing");
	put_label(18u, "Utilities");
	put_label(19u, "Other OpEx");
	put_label(20u, "Total OpEx");
	put_label(21u, "Operating profit");
	put_label(22u, "Operating margin");
	put_label(23u, "Tax rate");
	put_label(24u, "Taxes");
	put_label(25u, "Net income");
	put_label(26u, "Capital spending");
	put_label(27u, "Depreciation");
	put_label(28u, "Operating cash flow");
	put_label(29u, "Opening cash");
	put_label(30u, "Closing cash");
	put_label(32u, "LOAN SCHEDULE");
	put_label(33u, "Opening loan");
	put_label(34u, "Monthly rate");
	put_label(35u, "Payment");
	put_label(36u, "Interest");
	put_label(37u, "Principal");
	put_label(38u, "Closing loan");
	put_label(40u, "KEY PERFORMANCE INDICATORS");
	put_label(41u, "Revenue per unit");
	put_label(42u, "Variable cost/unit");
	put_label(43u, "Break-even units");
	put_label(44u, "Cash-positive flag");
	put_label(45u, "Debt coverage");
	put_label(46u, "Revenue growth");
	put_label(47u, "Profit growth");
	put_label(48u, "Cumulative income");
	put_label(49u, "Cumulative cash flow");
	put_label(50u, "Discounted cash flow");

	for (month = 0u; month < 12u; ++month) {
		unsigned char col = (unsigned char)(FIRST_MONTH_COL + month);
		char a[8], b[8], c[8];

		snprintf(text, sizeof(text), "%u", 950u + month * 55u + (month % 3u) * 25u);
		put_value(col, 3u, text, 0u);
		snprintf(text, sizeof(text), "%u.%02u", 49u + month / 4u,
			 (unsigned int)(95u + (month % 4u)) % 100u);
		put_value(col, 4u, text, 2u);
		put_binary(col, 5u, 3u, '*', 4u, 2u);
		snprintf(text, sizeof(text), "%u", 10500u + month * 475u);
		put_value(col, 6u, text, 2u);
		cell_name(a, col, 5u); cell_name(b, col, 6u);
		snprintf(text, sizeof(text), "=SUM(%s:%s)", a, b);
		put_value(col, 7u, text, 2u);
		snprintf(text, sizeof(text), "%u.%02u", 20u + month / 6u,
			 75u + (month % 4u) * 5u);
		put_value(col, 8u, text, 2u);
		put_binary(col, 9u, 3u, '*', 8u, 2u);
		cell_name(a, col, 6u);
		snprintf(text, sizeof(text), "=%s*0.35", a);
		put_value(col, 10u, text, 2u);
		cell_name(a, col, 9u); cell_name(b, col, 10u);
		snprintf(text, sizeof(text), "=SUM(%s:%s)", a, b);
		put_value(col, 11u, text, 2u);
		put_binary(col, 12u, 7u, '-', 11u, 2u);
		cell_name(a, col, 7u); cell_name(b, col, 12u);
		snprintf(text, sizeof(text), "=IF(%s=0,0,%s/%s)", a, b, a);
		put_value(col, 13u, text, 3u);

		snprintf(text, sizeof(text), "%u", 22500u + (month >= 6u ? 1800u : 0u));
		put_value(col, 15u, text, 2u);
		put_value(col, 16u, "4800", 2u);
		snprintf(text, sizeof(text), "%u", 4200u + (month % 3u) * 500u);
		put_value(col, 17u, text, 2u);
		snprintf(text, sizeof(text), "%u", 1250u + month * 35u);
		put_value(col, 18u, text, 2u);
		snprintf(text, sizeof(text), "%u", 900u + (month % 4u) * 125u);
		put_value(col, 19u, text, 2u);
		cell_name(a, col, 15u); cell_name(b, col, 19u);
		snprintf(text, sizeof(text), "=SUM(%s:%s)", a, b);
		put_value(col, 20u, text, 2u);
		put_binary(col, 21u, 12u, '-', 20u, 2u);
		cell_name(a, col, 7u); cell_name(b, col, 21u);
		snprintf(text, sizeof(text), "=IF(%s=0,0,%s/%s)", a, b, a);
		put_value(col, 22u, text, 3u);
		put_value(col, 23u, "0.24", 3u);
		cell_name(a, col, 21u); cell_name(b, col, 23u);
		snprintf(text, sizeof(text), "=IF(%s>0,%s*%s,0)", a, a, b);
		put_value(col, 24u, text, 2u);
		put_binary(col, 25u, 21u, '-', 24u, 2u);
		snprintf(text, sizeof(text), "%u", month % 4u == 0u ? 6000u : 1500u);
		put_value(col, 26u, text, 2u);
		put_value(col, 27u, "1800", 2u);
		cell_name(a, col, 25u); cell_name(b, col, 27u); cell_name(c, col, 26u);
		snprintf(text, sizeof(text), "=%s+%s-%s", a, b, c);
		put_value(col, 28u, text, 2u);

		if (month == 0u) {
			put_value(col, 29u, "50000", 2u);
			put_value(col, 33u, "100000", 2u);
		} else {
			cell_name(a, (unsigned char)(col - 1u), 30u);
			snprintf(text, sizeof(text), "=%s", a);
			put_value(col, 29u, text, 2u);
			cell_name(a, (unsigned char)(col - 1u), 38u);
			snprintf(text, sizeof(text), "=%s", a);
			put_value(col, 33u, text, 2u);
		}
		put_value(col, 34u, "0.006", 3u);
		cell_name(a, col, 33u); cell_name(b, col, 34u);
		snprintf(text, sizeof(text), "=IF(%s=0,0,-PMT(%s,60,%s))", a, b, a);
		put_value(col, 35u, text, 2u);
		put_binary(col, 36u, 33u, '*', 34u, 2u);
		put_binary(col, 37u, 35u, '-', 36u, 2u);
		cell_name(a, col, 33u); cell_name(b, col, 37u);
		snprintf(text, sizeof(text), "=IF(%s-%s<0,0,%s-%s)", a, b, a, b);
		put_value(col, 38u, text, 2u);
		cell_name(a, col, 29u); cell_name(b, col, 28u); cell_name(c, col, 35u);
		snprintf(text, sizeof(text), "=%s+%s-%s", a, b, c);
		put_value(col, 30u, text, 2u);

		cell_name(a, col, 3u); cell_name(b, col, 7u);
		snprintf(text, sizeof(text), "=IF(%s=0,0,%s/%s)", a, b, a);
		put_value(col, 41u, text, 2u);
		cell_name(a, col, 3u); cell_name(b, col, 11u);
		snprintf(text, sizeof(text), "=IF(%s=0,0,%s/%s)", a, b, a);
		put_value(col, 42u, text, 2u);
		cell_name(a, col, 4u); cell_name(b, col, 8u); cell_name(c, col, 20u);
		snprintf(text, sizeof(text), "=IF(%s-%s=0,0,%s/(%s-%s))", a, b, c, a, b);
		put_value(col, 43u, text, 0u);
		cell_name(a, col, 30u);
		snprintf(text, sizeof(text), "=IF(%s>0,1,0)", a);
		put_value(col, 44u, text, 0u);
		cell_name(a, col, 35u); cell_name(b, col, 28u);
		snprintf(text, sizeof(text), "=IF(%s=0,0,%s/%s)", a, b, a);
		put_value(col, 45u, text, 2u);
		if (month == 0u) {
			put_value(col, 46u, "0", 3u);
			put_value(col, 47u, "0", 3u);
			cell_name(a, col, 25u);
			snprintf(text, sizeof(text), "=%s", a);
			put_value(col, 48u, text, 2u);
			cell_name(a, col, 28u);
			snprintf(text, sizeof(text), "=%s", a);
			put_value(col, 49u, text, 2u);
		} else {
			char previous[8];

			cell_name(a, col, 7u); cell_name(previous, (unsigned char)(col - 1u), 7u);
			snprintf(text, sizeof(text), "=IF(%s=0,0,(%s-%s)/%s)", previous, a,
				 previous, previous);
			put_value(col, 46u, text, 3u);
			cell_name(a, col, 21u); cell_name(previous, (unsigned char)(col - 1u), 21u);
			snprintf(text, sizeof(text), "=IF(%s=0,0,(%s-%s)/ABS(%s))", previous,
				 a, previous, previous);
			put_value(col, 47u, text, 3u);
			cell_name(a, (unsigned char)(col - 1u), 48u); cell_name(b, col, 25u);
			snprintf(text, sizeof(text), "=%s+%s", a, b);
			put_value(col, 48u, text, 2u);
			cell_name(a, (unsigned char)(col - 1u), 49u); cell_name(b, col, 28u);
			snprintf(text, sizeof(text), "=%s+%s", a, b);
			put_value(col, 49u, text, 2u);
		}
		cell_name(a, col, 28u);
		snprintf(text, sizeof(text), "=%s*POWER(1.008,-%u)", a, month + 1u);
		put_value(col, 50u, text, 2u);
	}

	for (i = 0u; i < sizeof(total_rows) / sizeof(total_rows[0]); ++i) {
		put_total(total_rows[i]);
	}
	for (i = 0u; i < sizeof(average_rows) / sizeof(average_rows[0]); ++i) {
		put_average(average_rows[i], average_rows[i] == 43u ? 0u : 3u);
	}
	put_ending(29u, 2u);
	put_ending(30u, 2u);
	put_ending(33u, 2u);
	put_ending(38u, 2u);
	put_ending(48u, 2u);
	put_ending(49u, 2u);

	sheet_recalculate();
	for (i = 0u; i < sheet_count(); ++i) {
		struct sc_cell *cell = sheet_cell_at(i);

		if ((cell->flags & SC_FLAG_ERROR) != 0u) {
			fail_cell(cell->col, cell->row, sheet_text(cell), SC_ERR_FORMULA);
		}
	}
	saved_count = sheet_count();
	if (storage_save(path) != SC_OK) {
		fprintf(stderr, "generator: cannot save %s\n", path);
		return 1;
	}
	sheet_clear();
	if (storage_load(path) != SC_OK || sheet_count() != saved_count) {
		fprintf(stderr, "generator: round-trip validation failed for %s\n", path);
		return 1;
	}
	for (i = 0u; i < sheet_count(); ++i) {
		struct sc_cell *cell = sheet_cell_at(i);

		if ((cell->flags & SC_FLAG_ERROR) != 0u) {
			fail_cell(cell->col, cell->row, sheet_text(cell), SC_ERR_FORMULA);
		}
	}
	printf("Generated and verified %s: %u populated cells\n", path, sheet_count());
	return 0;
}
