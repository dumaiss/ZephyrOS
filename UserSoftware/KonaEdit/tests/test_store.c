#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ke_platform.h"
#include "ke_history.h"
#include "ke_file.h"
#include "ke_store.h"
#include "ke_text.h"

#define FAKE_OBJECTS 32

static uint8_t memory[FAKE_OBJECTS][32768];
static uint16_t sizes[FAKE_OBJECTS];
static uint8_t used[FAKE_OBJECTS];

#define FAKE_FILES 8
struct fake_file {
	char path[128];
	uint8_t data[65538];
	uint32_t length;
	uint8_t exists;
};
struct fake_open {
	uint8_t used;
	uint8_t file;
	uint32_t position;
};
static struct fake_file files[FAKE_FILES];
static struct fake_open opens[8];
static int rename_calls;
static int fail_rename_call;
static unsigned int xmem_write_calls;

static int find_file(const char *path)
{
	int i;

	for (i = 0; i < FAKE_FILES; ++i)
		if (files[i].exists && strcmp(files[i].path, path) == 0) return i;
	return -1;
}

static int add_file(const char *path, const uint8_t *data, uint32_t length)
{
	int i;

	for (i = 0; i < FAKE_FILES; ++i) if (!files[i].exists) break;
	assert(i < FAKE_FILES && length <= sizeof(files[i].data));
	files[i].exists = 1;
	strcpy(files[i].path, path);
	memcpy(files[i].data, data, length);
	files[i].length = length;
	return i;
}

int ke_xmem_alloc(uint16_t size, uint16_t flags)
{
	uint16_t i;

	(void)flags;
	for (i = 1; i < FAKE_OBJECTS; ++i) {
		if (!used[i]) {
			used[i] = 1;
			sizes[i] = size;
			memset(memory[i], 0xa5, sizeof(memory[i]));
			return i;
		}
	}
	return -1;
}

int ke_xmem_free(uint16_t handle)
{
	if (handle == 0 || handle >= FAKE_OBJECTS || !used[handle]) return -1;
	used[handle] = 0;
	return 0;
}

int ke_xmem_read(uint16_t handle, uint16_t offset, void *buffer,
		 uint16_t length)
{
	if (!used[handle] || (uint32_t)offset + length > sizes[handle]) return -1;
	memcpy(buffer, memory[handle] + offset, length);
	return length;
}

int ke_xmem_write(uint16_t handle, uint16_t offset, const void *buffer,
		  uint16_t length)
{
	if (!used[handle] || (uint32_t)offset + length > sizes[handle]) return -1;
	++xmem_write_calls;
	memcpy(memory[handle] + offset, buffer, length);
	return length;
}

int ke_fs_open(const char *path, uint8_t flags)
{
	int file = find_file(path);
	int handle;

	if (file >= 0 && (flags & KE_FS_EXCL)) return -16;
	if (file < 0) {
		if (!(flags & KE_FS_CREATE)) return -5;
		file = add_file(path, (const uint8_t *)"", 0);
	}
	if (flags & KE_FS_TRUNC) files[file].length = 0;
	for (handle = 1; handle < 8; ++handle) if (!opens[handle].used) break;
	if (handle == 8) return -23;
	opens[handle].used = 1;
	opens[handle].file = (uint8_t)file;
	opens[handle].position = 0;
	return handle;
}

int ke_fs_close(uint16_t handle)
{
	if (handle >= 8 || !opens[handle].used) return -2;
	opens[handle].used = 0;
	return 0;
}

int ke_fs_read(uint16_t handle, void *buffer, uint16_t length)
{
	struct fake_open *opened = &opens[handle];
	struct fake_file *file = &files[opened->file];
	uint32_t available = file->length - opened->position;

	if (available < length) length = (uint16_t)available;
	memcpy(buffer, file->data + opened->position, length);
	opened->position += length;
	return length;
}

int ke_fs_write(uint16_t handle, const void *buffer, uint16_t length)
{
	struct fake_open *opened = &opens[handle];
	struct fake_file *file = &files[opened->file];

	if (opened->position + length > sizeof(file->data)) return -6;
	memcpy(file->data + opened->position, buffer, length);
	opened->position += length;
	if (opened->position > file->length) file->length = opened->position;
	return length;
}

int ke_fs_sync(uint16_t handle)
{
	return opens[handle].used ? 0 : -2;
}

int ke_fs_stat(const char *path, struct ke_file_info *info)
{
	int file = find_file(path);

	if (file < 0) return -5;
	info->type = 1;
	info->size = files[file].length;
	return 0;
}

int ke_fs_unlink(const char *path)
{
	int file = find_file(path);

	if (file < 0) return -5;
	files[file].exists = 0;
	return 0;
}

int ke_fs_rename(const char *old_path, const char *new_path)
{
	int file = find_file(old_path);

	++rename_calls;
	if (fail_rename_call != 0 && rename_calls == fail_rename_call) return -10;
	if (file < 0 || find_file(new_path) >= 0) return -5;
	strcpy(files[file].path, new_path);
	return 0;
}

int ke_fs_list(const char *path, uint16_t *cookie,
	       struct ke_dir_entry *entries, uint16_t count)
{
	(void)path;
	(void)cookie;
	(void)entries;
	(void)count;
	return 0;
}

static void assert_bytes(struct ke_store *store, const uint8_t *expected,
			 size_t length)
{
	uint8_t block[257];
	size_t at = 0;

	assert(ke_store_length(store) == length);
	while (at < length) {
		uint16_t count = (uint16_t)(length - at > sizeof(block) ?
					 sizeof(block) : length - at);
		assert(ke_store_read(store, (ke_pos_t)at, block, count) == 0);
		assert(memcmp(block, expected + at, count) == 0);
		at += count;
	}
}

static void test_boundaries_and_capacity(void)
{
	struct ke_store store;
	uint8_t *expected = malloc(65537);
	uint8_t block[511];
	size_t at;

	assert(expected != NULL);
	for (at = 0; at < sizeof(block); ++at) block[at] = (uint8_t)at;
	assert(ke_store_init(&store) == 0);
	for (at = 0; at < 65536; at += sizeof(block)) {
		uint16_t count = (uint16_t)(65536 - at > sizeof(block) ?
					 sizeof(block) : 65536 - at);
		memcpy(expected + at, block, count);
		assert(ke_store_insert(&store, (ke_pos_t)at, block, count) == 0);
	}
	assert(ke_store_insert(&store, 65536, block, 1) == KE_STORE_ENOSPC);
	assert_bytes(&store, expected, 65536);
	assert(ke_store_delete(&store, 32767, 3) == 0);
	memmove(expected + 32767, expected + 32770, 65536 - 32770);
	assert_bytes(&store, expected, 65533);
	assert(ke_store_insert(&store, 32767, "XYZ", 3) == 0);
	memmove(expected + 32770, expected + 32767, 65533 - 32767);
	memcpy(expected + 32767, "XYZ", 3);
	assert_bytes(&store, expected, 65536);
	ke_store_dispose(&store);
	free(expected);
}

static void test_mid_page_splits(void)
{
	struct ke_store store;
	uint8_t expected[6000];
	uint8_t source[5000];
	size_t i;

	for (i = 0; i < sizeof(source); ++i) source[i] = (uint8_t)(i * 17u);
	memcpy(expected, source, sizeof(source));
	assert(ke_store_init(&store) == 0);
	for (i = 0; i < sizeof(source); i += 250) {
		assert(ke_store_insert(&store, i, source + i, 250) == 0);
	}
	for (i = 0; i < 800; ++i) {
		uint8_t value = (uint8_t)(255u - i);
		memmove(expected + 1001 + i, expected + 1000 + i, 5000 - 1000);
		expected[1000 + i] = value;
		assert(ke_store_insert(&store, 1000 + i, &value, 1) == 0);
	}
	assert_bytes(&store, expected, 5800);
	ke_store_dispose(&store);
}

static void test_history_and_clipboard(void)
{
	struct ke_store store;
	struct ke_history history;
	uint8_t value;

	assert(ke_store_init(&store) == 0);
	assert(ke_history_init(&history) == 0);
	assert(ke_history_edit(&history, &store, 0, 0,
			       (const uint8_t *)"alpha", 5) == 0);
	assert(ke_history_edit(&history, &store, 5, 0,
			       (const uint8_t *)"\tbeta", 5) == 0);
	assert(ke_history_copy(&history, &store, 0, 5) == 0);
	assert(ke_history_paste(&history, &store, 10) == 0);
	assert(ke_store_length(&store) == 15);
	assert(ke_history_undo(&history, &store) == 0);
	assert(ke_store_length(&store) == 10);
	assert(ke_history_undo(&history, &store) == 0);
	assert(ke_store_length(&store) == 5);
	assert(ke_history_redo(&history, &store) == 0);
	assert(ke_store_length(&store) == 10);
	assert(ke_history_cut(&history, &store, 0, 5) == 0);
	assert(ke_store_length(&store) == 5);
	assert(ke_history_undo(&history, &store) == 0);
	assert(ke_store_byte(&store, 0, &value) == 0 && value == 'a');
	assert(ke_history_redo(&history, &store) == 0);
	assert(ke_store_byte(&store, 0, &value) == 0 && value == '\t');
	ke_history_dispose(&history);
	ke_store_dispose(&store);
}

static void test_grouped_edits(void)
{
	struct ke_store store;
	struct ke_history history;
	uint8_t byte;

	assert(ke_store_init(&store) == 0);
	assert(ke_history_init(&history) == 0);
	assert(ke_history_edit_grouped(&history, &store, 0, 0,
		(const uint8_t *)"a", 1, KE_GROUP_TYPING) == 0);
	assert(ke_history_edit_grouped(&history, &store, 1, 0,
		(const uint8_t *)"b", 1, KE_GROUP_TYPING) == 0);
	assert(ke_history_edit_grouped(&history, &store, 2, 0,
		(const uint8_t *)"c", 1, KE_GROUP_TYPING) == 0);
	assert(history.count == 1);
	assert(ke_history_undo(&history, &store) == 0);
	assert(ke_store_length(&store) == 0);
	assert(ke_history_redo(&history, &store) == 0);
	assert(ke_history_edit_grouped(&history, &store, 2, 1, NULL, 0,
		KE_GROUP_BACKSPACE) == 0);
	assert(ke_history_edit_grouped(&history, &store, 1, 1, NULL, 0,
		KE_GROUP_BACKSPACE) == 0);
	assert(history.count == 2);
	assert(ke_history_undo(&history, &store) == 0);
	assert(ke_store_length(&store) == 3);
	assert(ke_store_byte(&store, 1, &byte) == 0 && byte == 'b');
	assert(ke_history_edit_grouped(&history, &store, 0, 1, NULL, 0,
		KE_GROUP_DELETE) == 0);
	assert(ke_history_edit_grouped(&history, &store, 0, 1, NULL, 0,
		KE_GROUP_DELETE) == 0);
	assert(ke_history_undo(&history, &store) == 0);
	assert_bytes(&store, (const uint8_t *)"abc", 3);
	ke_history_dispose(&history);
	ke_store_dispose(&store);
}

static void test_typing_history_staging(void)
{
	struct ke_store store;
	struct ke_history history;
	uint8_t byte = 'x';
	unsigned int i;

	assert(ke_store_init(&store) == 0);
	assert(ke_history_init(&history) == 0);
	xmem_write_calls = 0u;
	for (i = 0u; i < 25u; ++i)
		assert(ke_history_edit_grouped(&history, &store, i, 0ul, &byte,
			1u, KE_GROUP_TYPING) == 0);
	assert(xmem_write_calls == 1u);
	assert(ke_history_undo(&history, &store) == 0);
	assert(xmem_write_calls == 2u);
	assert(ke_store_length(&store) == 0ul);
	assert(ke_history_redo(&history, &store) == 0);
	assert(ke_store_length(&store) == 25ul);
	ke_history_dispose(&history);
	ke_store_dispose(&store);
}

static void test_text_semantics(void)
{
	struct ke_store store;
	struct ke_normalizer normalizer = {0};
	uint8_t input[] = {'A', 13, 10, 'B', 13, 'C', 9, 0x80};
	uint8_t cpm_input[] = {'X', 13, 0x1a, 0, 'J'};
	uint8_t normalized[16];
	uint16_t consumed;
	uint16_t produced;
	ke_pos_t found;

	assert(ke_normalize(&normalizer, input, sizeof(input), normalized,
			    sizeof(normalized), &consumed, &produced) == 0);
	assert(consumed == sizeof(input));
	assert(produced == 7);
	assert(memcmp(normalized, "A\nB\nC\t\x80", 7) == 0);
	assert(ke_visual_advance(0, 9) == 8);
	assert(ke_visual_advance(3, 9) == 8);
	assert(ke_visual_advance(8, 9) == 16);
	assert(ke_store_init(&store) == 0);
	assert(ke_store_insert(&store, 0, normalized, produced) == 0);
	assert(ke_line_start(&store, 5) == 4);
	assert(ke_line_end(&store, 4) == 7);
	assert(ke_visual_column(&store, 6) == 8);
	assert(ke_position_for_column(&store, 4, 7) == 5);
	assert(ke_position_for_column(&store, 4, 8) == 6);
	assert(ke_find(&store, 0, (const uint8_t *)"b\nC", 3, 0, &found));
	assert(found == 2);
	ke_store_dispose(&store);
	memset(&normalizer, 0, sizeof(normalizer));
	assert(ke_normalize(&normalizer, cpm_input, sizeof(cpm_input),
			    normalized, sizeof(normalized), &consumed,
			    &produced) == 0);
	assert(consumed == 3);
	assert(produced == 2);
	assert(normalizer.eof != 0u);
	assert(memcmp(normalized, "X\n", 2) == 0);
}

static void test_file_load_save_and_rollback(void)
{
	struct ke_store store;
	const uint8_t source[] = "one\r\ntwo\rthree\t\x80";
	const uint8_t cpm_source[] = {
		'M', 'O', 'D', 13, 10, 0x1a, 0x1a, 0, 'D', 'I', 'R'
	};
	uint8_t readback[32];
	char recovery[128];
	int file;

	memset(files, 0, sizeof(files));
	memset(opens, 0, sizeof(opens));
	add_file("/fs/INPUT.TXT", source, sizeof(source) - 1);
	assert(ke_store_init(&store) == 0);
	assert(ke_file_load(&store, "/fs/INPUT.TXT") == 0);
	assert(ke_store_length(&store) == 15);
	assert(ke_store_read(&store, 0, readback, 15) == 0);
	assert(memcmp(readback, "one\ntwo\nthree\t\x80", 15) == 0);
	assert(ke_file_save(&store, "/fs/OUTPUT.TXT", recovery,
			    sizeof(recovery)) == 0);
	file = find_file("/fs/OUTPUT.TXT");
	assert(file >= 0 && files[file].length == 15);
	assert(memcmp(files[file].data, readback, 15) == 0);
	add_file("/fs/LIST.MOD", cpm_source, sizeof(cpm_source));
	ke_store_dispose(&store);
	assert(ke_store_init(&store) == 0);
	assert(ke_file_load(&store, "/fs/LIST.MOD") == 0);
	assert(ke_store_length(&store) == 4);
	assert(ke_store_read(&store, 0, readback, 4) == 0);
	assert(memcmp(readback, "MOD\n", 4) == 0);

	memcpy(files[file].data, "old", 3);
	files[file].length = 3;
	rename_calls = 0;
	fail_rename_call = 2;
	assert(ke_file_save(&store, "/fs/OUTPUT.TXT", recovery,
			    sizeof(recovery)) < 0);
	file = find_file("/fs/OUTPUT.TXT");
	assert(file >= 0 && files[file].length == 3);
	assert(memcmp(files[file].data, "old", 3) == 0);
	fail_rename_call = 0;
	ke_store_dispose(&store);
}

int main(void)
{
	test_boundaries_and_capacity();
	test_mid_page_splits();
	test_history_and_clipboard();
	test_grouped_edits();
	test_typing_history_staging();
	test_text_semantics();
	test_file_load_save_and_rollback();
	puts("KonaEdit store tests: OK");
	return 0;
}
