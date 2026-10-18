#include "sc2kos.h"

static const unsigned char file_magic[4] = { 'K', 'S', 'C', '1' };

#if defined(__SDCC)

#define STORAGE_BUFFER_SIZE 128u

struct storage_writer {
	int handle;
	unsigned char data[STORAGE_BUFFER_SIZE];
	unsigned int length;
	int error;
};

struct storage_reader {
	int handle;
	unsigned char data[STORAGE_BUFFER_SIZE];
	unsigned int at;
	unsigned int length;
	int error;
};

static void writer_flush(struct storage_writer *writer)
{
	unsigned int at = 0u;

	while (writer->error == SC_OK && at < writer->length) {
		int result = write((unsigned int)writer->handle, &writer->data[at],
				   writer->length - at);

		if (result <= 0) { writer->error = SC_ERR_IO; break; }
		at += (unsigned int)result;
	}
	writer->length = 0u;
}

static void writer_put(struct storage_writer *writer, const void *data,
		       unsigned int length)
{
	const unsigned char *source = (const unsigned char *)data;

	while (writer->error == SC_OK && length != 0u) {
		unsigned int room = STORAGE_BUFFER_SIZE - writer->length;
		unsigned int amount = length < room ? length : room;

		memcpy(&writer->data[writer->length], source, amount);
		writer->length += amount;
		source += amount;
		length -= amount;
		if (writer->length == STORAGE_BUFFER_SIZE) { writer_flush(writer); }
	}
}

static void writer_char(struct storage_writer *writer, unsigned char ch)
{
	writer_put(writer, &ch, 1u);
}

static void writer_text(struct storage_writer *writer, const char *text)
{
	writer_put(writer, text, strlen(text));
}

static void reader_get(struct storage_reader *reader, void *data,
		       unsigned int length)
{
	unsigned char *target = (unsigned char *)data;

	while (reader->error == SC_OK && length != 0u) {
		unsigned int available;
		unsigned int amount;

		if (reader->at == reader->length) {
			int result = read((unsigned int)reader->handle, reader->data,
					  STORAGE_BUFFER_SIZE);

			if (result <= 0) { reader->error = SC_ERR_FORMAT; break; }
			reader->at = 0u;
			reader->length = (unsigned int)result;
		}
		available = reader->length - reader->at;
		amount = length < available ? length : available;
		memcpy(target, &reader->data[reader->at], amount);
		reader->at += amount;
		target += amount;
		length -= amount;
	}
}

int storage_save(const char *path)
{
	struct storage_writer writer;
	unsigned char header[9];
	unsigned int i;

	writer.handle = open(path, KFS_O_WRITE | KFS_O_CREATE | KFS_O_TRUNC);
	if (writer.handle < 0) { return SC_ERR_IO; }
	writer.length = 0u;
	writer.error = SC_OK;
	memcpy(header, file_magic, sizeof(file_magic));
	header[4] = 1u;
	header[5] = (unsigned char)(sheet_count() & 0xffu);
	header[6] = (unsigned char)(sheet_count() >> 8);
	header[7] = SC_MAX_COLS;
	header[8] = SC_MAX_ROWS;
	writer_put(&writer, header, sizeof(header));
	for (i = 0u; writer.error == SC_OK && i < sheet_count(); ++i) {
		struct sc_cell *cell = sheet_cell_at(i);
		unsigned char record[7];

		record[0] = cell->col;
		record[1] = cell->row;
		record[2] = cell->type;
		record[3] = (unsigned char)(cell->flags & SC_FLAG_PROTECTED);
		record[4] = cell->precision;
		record[5] = cell->text_length;
		record[6] = 0u;
		writer_put(&writer, record, sizeof(record));
		writer_put(&writer, sheet_text(cell), cell->text_length);
	}
	writer_flush(&writer);
	if (writer.error == SC_OK && sync((unsigned int)writer.handle) < 0) {
		writer.error = SC_ERR_IO;
	}
	if (close((unsigned int)writer.handle) < 0) { writer.error = SC_ERR_IO; }
	return writer.error;
}

static int storage_read_file(const char *path)
{
	struct storage_reader reader;
	unsigned char header[9];
	unsigned int count;
	unsigned int i;
	int result = SC_OK;

	reader.handle = open(path, KFS_O_READ);
	if (reader.handle < 0) { return SC_ERR_IO; }
	reader.at = 0u;
	reader.length = 0u;
	reader.error = SC_OK;
	reader_get(&reader, header, sizeof(header));
	if (reader.error != SC_OK || memcmp(header, file_magic, sizeof(file_magic)) != 0 ||
	    header[4] != 1u || header[7] != SC_MAX_COLS || header[8] != SC_MAX_ROWS) {
		(void)close((unsigned int)reader.handle);
		return SC_ERR_FORMAT;
	}
	count = (unsigned int)header[5] | ((unsigned int)header[6] << 8);
	if (count > SC_MAX_CELLS) {
		(void)close((unsigned int)reader.handle);
		return SC_ERR_FULL;
	}
	sheet_clear();
	for (i = 0u; result == SC_OK && i < count; ++i) {
		unsigned char record[7];
		char text[SC_INPUT_MAX + 1u];

		reader_get(&reader, record, sizeof(record));
		if (reader.error != SC_OK || record[5] > SC_INPUT_MAX) {
			result = SC_ERR_FORMAT;
			break;
		}
		reader_get(&reader, text, record[5]);
		if (reader.error != SC_OK) { result = SC_ERR_FORMAT; break; }
		text[record[5]] = '\0';
		if (sheet_import(record[0], record[1], record[2], record[3],
				 record[4], text) < 0) {
			result = SC_ERR_FORMAT;
		}
	}
	if (close((unsigned int)reader.handle) < 0 && result == SC_OK) {
		result = SC_ERR_IO;
	}
	return result;
}

static void csv_put_quoted(struct storage_writer *writer, const char *text)
{
	const char *scan;

	writer_char(writer, '"');
	for (scan = text; *scan != '\0'; ++scan) {
		if (*scan == '"') { writer_char(writer, '"'); }
		writer_char(writer, (unsigned char)*scan);
	}
	writer_char(writer, '"');
}

int storage_export_csv(const char *path)
{
	struct storage_writer writer;
	unsigned char last_col, last_row;
	unsigned int row, col;

	writer.handle = open(path, KFS_O_WRITE | KFS_O_CREATE | KFS_O_TRUNC);
	if (writer.handle < 0) { return SC_ERR_IO; }
	writer.length = 0u;
	writer.error = SC_OK;
	sheet_used(&last_col, &last_row);
	for (row = 0u; writer.error == SC_OK && row <= (unsigned int)last_row; ++row) {
		for (col = 0u; col <= (unsigned int)last_col; ++col) {
			struct sc_cell *cell = sheet_find((unsigned char)col,
						       (unsigned char)row);

			if (col != 0u) { writer_char(&writer, ','); }
			if (cell != (struct sc_cell *)0) {
				if (cell->type == SC_CELL_TEXT) {
					csv_put_quoted(&writer, sheet_text(cell));
				} else if (cell->flags & SC_FLAG_ERROR) {
					writer_text(&writer, "#ERROR");
				} else {
					char number[24];

					sheet_format_value(number, sizeof(number), cell->value,
							   cell->precision);
					writer_text(&writer, number);
				}
			}
		}
		writer_text(&writer, "\r\n");
	}
	writer_flush(&writer);
	if (writer.error == SC_OK && sync((unsigned int)writer.handle) < 0) {
		writer.error = SC_ERR_IO;
	}
	if (close((unsigned int)writer.handle) < 0) { writer.error = SC_ERR_IO; }
	return writer.error;
}

#else

static int write_all(const void *ptr, unsigned int size, FILE *stream)
{
	return fwrite(ptr, 1u, size, stream) == size ? SC_OK : SC_ERR_IO;
}

int storage_save(const char *path)
{
	FILE *stream = fopen(path, "wb");
	unsigned char header[9];
	unsigned int i;
	int result = SC_OK;

	if (stream == (FILE *)0) { return SC_ERR_IO; }
	memcpy(header, file_magic, sizeof(file_magic));
	header[4] = 1u;
	header[5] = (unsigned char)(sheet_count() & 0xffu);
	header[6] = (unsigned char)(sheet_count() >> 8);
	header[7] = SC_MAX_COLS;
	header[8] = SC_MAX_ROWS;
	if (write_all(header, sizeof(header), stream) < 0) { result = SC_ERR_IO; }
	for (i = 0u; result == SC_OK && i < sheet_count(); ++i) {
		struct sc_cell *cell = sheet_cell_at(i);
		unsigned char record[7];

		record[0] = cell->col;
		record[1] = cell->row;
		record[2] = cell->type;
		record[3] = (unsigned char)(cell->flags & SC_FLAG_PROTECTED);
		record[4] = cell->precision;
		record[5] = cell->text_length;
		record[6] = 0u;
		if (write_all(record, sizeof(record), stream) < 0 ||
		    write_all(sheet_text(cell), cell->text_length, stream) < 0) {
			result = SC_ERR_IO;
		}
	}
	if (fclose(stream) != 0) { result = SC_ERR_IO; }
	return result;
}

static int storage_read_file(const char *path)
{
	FILE *stream = fopen(path, "rb");
	unsigned char header[9];
	unsigned int count;
	unsigned int i;
	int result = SC_OK;

	if (stream == (FILE *)0) { return SC_ERR_IO; }
	if (fread(header, 1u, sizeof(header), stream) != sizeof(header) ||
	    memcmp(header, file_magic, sizeof(file_magic)) != 0 ||
	    header[4] != 1u || header[7] != SC_MAX_COLS ||
	    header[8] != SC_MAX_ROWS) {
		(void)fclose(stream);
		return SC_ERR_FORMAT;
	}
	count = (unsigned int)header[5] | ((unsigned int)header[6] << 8);
	if (count > SC_MAX_CELLS) {
		(void)fclose(stream);
		return SC_ERR_FULL;
	}
	sheet_clear();
	for (i = 0u; i < count; ++i) {
		unsigned char record[7];
		char text[SC_INPUT_MAX + 1u];

		if (fread(record, 1u, sizeof(record), stream) != sizeof(record) ||
		    record[5] > SC_INPUT_MAX ||
		    fread(text, 1u, record[5], stream) != record[5]) {
			result = SC_ERR_FORMAT;
			break;
		}
		text[record[5]] = '\0';
		if (sheet_import(record[0], record[1], record[2], record[3],
				 record[4], text) < 0) {
			result = SC_ERR_FORMAT;
			break;
		}
	}
	if (fclose(stream) != 0 && result == SC_OK) { result = SC_ERR_IO; }
	return result;
}

static int csv_put_quoted(FILE *stream, const char *text)
{
	const char *scan;

	if (fputc('"', stream) == EOF) { return SC_ERR_IO; }
	for (scan = text; *scan != '\0'; ++scan) {
		if (*scan == '"' && fputc('"', stream) == EOF) { return SC_ERR_IO; }
		if (fputc((unsigned char)*scan, stream) == EOF) { return SC_ERR_IO; }
	}
	return fputc('"', stream) == EOF ? SC_ERR_IO : SC_OK;
}

int storage_export_csv(const char *path)
{
	FILE *stream = fopen(path, "wb");
	unsigned char last_col, last_row;
	unsigned int row, col;
	int result = SC_OK;

	if (stream == (FILE *)0) { return SC_ERR_IO; }
	sheet_used(&last_col, &last_row);
	for (row = 0u; result == SC_OK && row <= (unsigned int)last_row; ++row) {
		for (col = 0u; col <= (unsigned int)last_col; ++col) {
			struct sc_cell *cell = sheet_find((unsigned char)col,
						       (unsigned char)row);
			if (col != 0u && fputc(',', stream) == EOF) {
				result = SC_ERR_IO;
				break;
			}
			if (cell != (struct sc_cell *)0) {
				if (cell->type == SC_CELL_TEXT) {
					result = csv_put_quoted(stream, sheet_text(cell));
				} else if (cell->flags & SC_FLAG_ERROR) {
					result = fputs("#ERROR", stream) == EOF ? SC_ERR_IO : SC_OK;
				} else {
					char number[24];
					sheet_format_value(number, sizeof(number), cell->value,
							   cell->precision);
					result = fputs(number, stream) == EOF ? SC_ERR_IO : SC_OK;
				}
			}
		}
		if (result == SC_OK && fputs("\r\n", stream) == EOF) {
			result = SC_ERR_IO;
		}
	}
	if (fclose(stream) != 0) { result = SC_ERR_IO; }
	return result;
}

#endif

int storage_load(const char *path)
{
	int result = storage_read_file(path);

	/* The target reader and its per-record text buffer occupy a meaningful
	 * fraction of the Z80 stack.  Recalculate only after that helper returns,
	 * so formula recursion gets the complete application stack. */
	if (result == SC_OK) { sheet_recalculate(); }
	return result;
}
