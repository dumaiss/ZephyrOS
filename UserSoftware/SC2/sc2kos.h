#ifndef SC2KOS_H
#define SC2KOS_H

#include "kos.h"

#define SC_MAX_COLS       63u
#define SC_MAX_ROWS       254u
#define SC_MAX_CELLS      768u
#define SC_TEXT_POOL_SIZE 12288u
#define SC_INPUT_MAX      63u
#define SC_PATH_MAX       127u
#define SC_SCALE          1000L

#define SC_CELL_TEXT      1u
#define SC_CELL_NUMBER    2u
#define SC_CELL_FORMULA   3u

#define SC_FLAG_PROTECTED 0x01u
#define SC_FLAG_ERROR     0x02u

#define SC_OK              0
#define SC_ERR_FULL       -1
#define SC_ERR_FORMULA    -2
#define SC_ERR_PROTECTED  -3
#define SC_ERR_RANGE      -4
#define SC_ERR_IO         -5
#define SC_ERR_FORMAT     -6

struct sc_cell {
	unsigned char col;
	unsigned char row;
	unsigned char type;
	unsigned char flags;
	unsigned char precision;
	unsigned int text_offset;
	unsigned char text_length;
	long value;
};

void sheet_clear(void);
unsigned int sheet_count(void);
struct sc_cell *sheet_cell_at(unsigned int index);
struct sc_cell *sheet_find(unsigned char col, unsigned char row);
const char *sheet_text(const struct sc_cell *cell);
int sheet_set(unsigned char col, unsigned char row, const char *text);
int sheet_import(unsigned char col, unsigned char row, unsigned char type,
		 unsigned char flags, unsigned char precision, const char *text);
int sheet_blank(unsigned char c1, unsigned char r1,
		unsigned char c2, unsigned char r2);
int sheet_copy(unsigned char src_col, unsigned char src_row,
	       unsigned char dst_col, unsigned char dst_row, unsigned char move);
int sheet_replicate(unsigned char src_col, unsigned char src_row,
		    unsigned char c1, unsigned char r1,
		    unsigned char c2, unsigned char r2);
int sheet_insert_row(unsigned char row);
int sheet_insert_col(unsigned char col);
int sheet_delete_row(unsigned char row);
int sheet_delete_col(unsigned char col);
void sheet_recalculate(void);
void sheet_used(unsigned char *last_col, unsigned char *last_row);
void sheet_format_value(char *dst, unsigned int size, long value,
			unsigned char precision);
int sheet_parse_ref(const char *text, unsigned char *col, unsigned char *row,
		    const char **end);
int sheet_parse_range(const char *text, unsigned char *c1, unsigned char *r1,
		      unsigned char *c2, unsigned char *r2);
void sheet_ref(char *dst, unsigned char col, unsigned char row);

int storage_save(const char *path);
int storage_load(const char *path);
int storage_export_csv(const char *path);

#endif
