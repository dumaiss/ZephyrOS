#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "sc2kos.h"

static struct sc_cell *cell(unsigned char col, unsigned char row)
{
	struct sc_cell *result = sheet_find(col, row);

	assert(result != NULL);
	return result;
}

static void test_references(void)
{
	unsigned char col, row;
	const char *end;
	char name[8];

	assert(sheet_parse_ref("A1", &col, &row, &end));
	assert(col == 0u && row == 0u && *end == '\0');
	assert(sheet_parse_ref("BK254", &col, &row, &end));
	assert(col == 62u && row == 253u && *end == '\0');
	assert(sheet_parse_ref("$BK$254", &col, &row, &end));
	assert(col == 62u && row == 253u && *end == '\0');
	assert(!sheet_parse_ref("BL1", &col, &row, &end));
	sheet_ref(name, 62u, 253u);
	assert(strcmp(name, "BK254") == 0);
}

static void test_formulae(void)
{
	char formatted[24];
	int result;

	sheet_clear();
	assert(sheet_set(0u, 0u, "10") == SC_OK);
	assert(sheet_set(1u, 0u, "2.5") == SC_OK);
	result = sheet_set(2u, 0u, "=A1+B1");
	if (result != SC_OK) {
		fprintf(stderr, "A1=%s:%ld/%u B1=%s:%ld/%u C1=%s:%ld/%u result=%d\n",
			sheet_text(cell(0u, 0u)), cell(0u, 0u)->value, cell(0u, 0u)->flags,
			sheet_text(cell(1u, 0u)), cell(1u, 0u)->value, cell(1u, 0u)->flags,
			sheet_text(cell(2u, 0u)), cell(2u, 0u)->value, cell(2u, 0u)->flags, result);
	}
	assert(result == SC_OK);
	assert(cell(2u, 0u)->value == 12500L);
	assert(sheet_set(3u, 0u, "=A1*B1") == SC_OK);
	assert(cell(3u, 0u)->value == 25000L);
	assert(sheet_set(4u, 0u, "=SUM(A1:D1)") == SC_OK);
	assert(cell(4u, 0u)->value == 50000L);
	assert(sheet_set(5u, 0u, "=AVERAGE(A1:B1)") == SC_OK);
	assert(cell(5u, 0u)->value == 6250L);
	sheet_format_value(formatted, sizeof(formatted), -6250L, 3u);
	assert(strcmp(formatted, "-6.25") == 0);
	assert(sheet_set(0u, 1u, "=B2") == SC_OK);
	assert(sheet_set(1u, 1u, "=A2") == SC_ERR_FORMULA);
	assert(cell(0u, 1u)->flags & SC_FLAG_ERROR);
}

static void test_edits_and_storage(void)
{
	struct sc_cell *protected_cell;
	FILE *csv;
	char output[128];
	size_t length;

	sheet_clear();
	assert(sheet_set(0u, 0u, "'Label") == SC_OK);
	assert(sheet_set(1u, 0u, "42") == SC_OK);
	assert(sheet_copy(1u, 0u, 1u, 1u, 0u) == SC_OK);
	protected_cell = cell(1u, 1u);
	protected_cell->flags |= SC_FLAG_PROTECTED;
	assert(sheet_blank(0u, 0u, 1u, 1u) == SC_ERR_PROTECTED);
	assert(sheet_find(0u, 0u) != NULL);
	assert(sheet_copy(1u, 1u, 2u, 1u, 1u) == SC_ERR_PROTECTED);
	assert(sheet_find(2u, 1u) == NULL);
	assert(storage_save("build/tests/roundtrip.ksc") == SC_OK);
	sheet_clear();
	assert(storage_load("build/tests/roundtrip.ksc") == SC_OK);
	assert(strcmp(sheet_text(cell(0u, 0u)), "Label") == 0);
	assert(cell(1u, 1u)->flags & SC_FLAG_PROTECTED);
	assert(storage_export_csv("build/tests/roundtrip.csv") == SC_OK);
	csv = fopen("build/tests/roundtrip.csv", "rb");
	assert(csv != NULL);
	length = fread(output, 1u, sizeof(output) - 1u, csv);
	output[length] = '\0';
	assert(fclose(csv) == 0);
	assert(strcmp(output, "\"Label\",42\r\n,42\r\n") == 0);
}

static void test_practical_formulae(void)
{
	struct sc_cell *result;

	sheet_clear();
	assert(sheet_set(0u, 0u, ".5") == SC_OK);
	assert(cell(0u, 0u)->value == 500L);
	assert(sheet_set(1u, 0u, "=ROUND(12.345,2)") == SC_OK);
	assert(cell(1u, 0u)->value == 12350L);
	assert(sheet_set(2u, 0u, "=ABS(-4.25)+INT(2.9)") == SC_OK);
	assert(cell(2u, 0u)->value == 6250L);
	assert(sheet_set(3u, 0u, "=MOD(17,5)+POWER(2,3)") == SC_OK);
	assert(cell(3u, 0u)->value == 10000L);
	assert(sheet_set(9u, 0u, "=SQRT(6.25)") == SC_OK);
	assert(cell(9u, 0u)->value == 2500L);
	assert(sheet_set(4u, 0u, "=IF(A1<1,AND(1,NOT(0)),0)") == SC_OK);
	assert(cell(4u, 0u)->value == SC_SCALE);
	assert(sheet_set(5u, 0u, "=YEAR(DATE(2026,10,16))") == SC_OK);
	assert(cell(5u, 0u)->value == 2026L * SC_SCALE);
	assert(sheet_set(6u, 0u, "=MONTH(DATE(2026,10,16))") == SC_OK);
	assert(cell(6u, 0u)->value == 10L * SC_SCALE);
	assert(sheet_set(7u, 0u, "=DAY(DATE(2026,10,16))") == SC_OK);
	assert(cell(7u, 0u)->value == 16L * SC_SCALE);
	assert(sheet_set(8u, 0u, "=PMT(0,12,1200)") == SC_OK);
	assert(cell(8u, 0u)->value == -100L * SC_SCALE);

	assert(sheet_set(0u, 1u, "10") == SC_OK);
	assert(sheet_set(1u, 1u, "=A2") == SC_OK);
	assert(sheet_copy(1u, 1u, 2u, 2u, 0u) == SC_OK);
	result = cell(2u, 2u);
	assert(strcmp(sheet_text(result), "=B3") == 0);
	assert(sheet_set(1u, 2u, "7") == SC_OK);
	assert(cell(2u, 2u)->value == 7000L);
	assert(sheet_replicate(1u, 1u, 2u, 1u, 2u, 3u) == SC_OK);
	assert(strcmp(sheet_text(cell(2u, 1u)), "=B2") == 0);
	assert(strcmp(sheet_text(cell(2u, 3u)), "=B4") == 0);
	assert(sheet_set(3u, 1u, "=$A2+A$1+$A$1") == SC_OK);
	assert(sheet_copy(3u, 1u, 4u, 2u, 0u) == SC_OK);
	assert(strcmp(sheet_text(cell(4u, 2u)), "=$A3+B$1+$A$1") == 0);
}

static void test_capacity(void)
{
	unsigned int i;

	sheet_clear();
	for (i = 0u; i < SC_MAX_CELLS; ++i) {
		unsigned char col = (unsigned char)(i % SC_MAX_COLS);
		unsigned char row = (unsigned char)(i / SC_MAX_COLS);

		assert(sheet_set(col, row, "1") == SC_OK);
	}
	assert(sheet_count() == SC_MAX_CELLS);
	assert(storage_save("build/tests/capacity.ksc") == SC_OK);
	sheet_clear();
	assert(storage_load("build/tests/capacity.ksc") == SC_OK);
	assert(sheet_count() == SC_MAX_CELLS);
	assert(cell((unsigned char)((SC_MAX_CELLS - 1u) % SC_MAX_COLS),
		    (unsigned char)((SC_MAX_CELLS - 1u) / SC_MAX_COLS))->value == SC_SCALE);
	assert(sheet_set((unsigned char)(SC_MAX_CELLS % SC_MAX_COLS),
			 (unsigned char)(SC_MAX_CELLS / SC_MAX_COLS), "1") == SC_ERR_FULL);
}

int main(void)
{
	test_references();
	test_formulae();
	test_edits_and_storage();
	test_practical_formulae();
	test_capacity();
	puts("SC2Kos host tests: PASS");
	return 0;
}
