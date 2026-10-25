/* KRES host tests: the instrumented in-memory adapter. */

#include <stdlib.h>
#include <string.h>

#include "kres_mem_io.h"

#define MEM_MAX_FILES 8u
#define MEM_MAX_SLOTS 4u
#define MEM_NO_FAULT 0xFFFFFFFFu

struct mem_file mem_files[MEM_MAX_FILES];
static unsigned int mem_file_count;

struct mem_slot {
	void *ctx;
	unsigned int file; /* MEM_MAX_FILES when free */
	uint32_t cursor;
};

static struct mem_slot mem_slots[MEM_MAX_SLOTS];

static unsigned int write_count;
static unsigned int read_count;
static unsigned int seek_count;
static unsigned int sync_count;
static unsigned int open_count;

static int refuse_growth;

void mem_truncate_refuses_growth(int on)
{
	refuse_growth = on;
}

static unsigned int fail_write_ord = MEM_NO_FAULT;
static int fail_write_code;
static unsigned int fail_read_ord = MEM_NO_FAULT;
static int fail_read_code;
static unsigned int short_read_ord = MEM_NO_FAULT;
static unsigned int fail_seek_ord = MEM_NO_FAULT;
static int fail_seek_code;
static unsigned int fail_sync_ord = MEM_NO_FAULT;
static int fail_sync_code;

void mem_io_reset(void)
{
	unsigned int i;

	for (i = 0u; i < mem_file_count; ++i) {
		free(mem_files[i].data);
	}
	memset(mem_files, 0, sizeof(mem_files));
	mem_file_count = 0u;
	memset(mem_slots, 0, sizeof(mem_slots));
	for (i = 0u; i < MEM_MAX_SLOTS; ++i) {
		mem_slots[i].file = MEM_MAX_FILES;
	}
	write_count = 0u;
	read_count = 0u;
	seek_count = 0u;
	sync_count = 0u;
	open_count = 0u;
	fail_write_ord = MEM_NO_FAULT;
	fail_read_ord = MEM_NO_FAULT;
	short_read_ord = MEM_NO_FAULT;
	fail_seek_ord = MEM_NO_FAULT;
	fail_sync_ord = MEM_NO_FAULT;
}

unsigned int mem_count_writes(void)
{
	return write_count;
}

unsigned int mem_count_reads(void)
{
	return read_count;
}

unsigned int mem_count_seeks(void)
{
	return seek_count;
}

unsigned int mem_count_syncs(void)
{
	return sync_count;
}

void mem_fail_write(unsigned int ordinal, int code)
{
	fail_write_ord = ordinal;
	fail_write_code = code;
}

void mem_fail_read(unsigned int ordinal, int code)
{
	fail_read_ord = ordinal;
	fail_read_code = code;
}

void mem_short_read(unsigned int ordinal)
{
	short_read_ord = ordinal;
}

void mem_fail_seek(unsigned int ordinal, int code)
{
	fail_seek_ord = ordinal;
	fail_seek_code = code;
}

void mem_fail_sync(unsigned int ordinal, int code)
{
	fail_sync_ord = ordinal;
	fail_sync_code = code;
}

static struct mem_file *find_file(const char *path)
{
	unsigned int i;

	for (i = 0u; i < mem_file_count; ++i) {
		if (strcmp(mem_files[i].name, path) == 0) {
			return &mem_files[i];
		}
	}
	return (struct mem_file *)0;
}

const struct mem_file *mem_file_by_name(const char *path)
{
	return find_file(path);
}

void mem_file_new(const char *path, const uint8_t *data, uint32_t size)
{
	struct mem_file *f = find_file(path);

	if (f == (struct mem_file *)0) {
		f = &mem_files[mem_file_count++];
		memset(f, 0, sizeof(*f));
		strncpy(f->name, path, sizeof(f->name) - 1u);
	}
	free(f->data);
	f->data = (uint8_t *)malloc(size != 0u ? size : 1u);
	memcpy(f->data, data, size);
	f->size = size;
	f->capacity = size;
}

int mem_file_put(const char *path, const uint8_t *data, uint32_t size)
{
	struct mem_file *f = find_file(path);

	if (f == (struct mem_file *)0) {
		return -1;
	}
	mem_file_new(path, data, size);
	return 0;
}

static struct mem_slot *find_slot(void *ctx)
{
	unsigned int i;

	for (i = 0u; i < MEM_MAX_SLOTS; ++i) {
		if (mem_slots[i].ctx == ctx &&
		    mem_slots[i].file != MEM_MAX_FILES) {
			return &mem_slots[i];
		}
	}
	return (struct mem_slot *)0;
}

static struct mem_slot *take_slot(void *ctx)
{
	struct mem_slot *slot = find_slot(ctx);
	unsigned int i;

	if (slot != (struct mem_slot *)0) {
		return slot;
	}
	for (i = 0u; i < MEM_MAX_SLOTS; ++i) {
		if (mem_slots[i].file == MEM_MAX_FILES) {
			mem_slots[i].ctx = ctx;
			return &mem_slots[i];
		}
	}
	return (struct mem_slot *)0;
}

static int mem_open(void *ctx, const char *path, unsigned int flags)
{
	struct mem_file *f = find_file(path);
	struct mem_slot *slot;

	if (f == (struct mem_file *)0) {
		if ((flags & KRES_OPEN_CREATE) == 0u) {
			return -KOS_ENOENT;
		}
		if (mem_file_count >= MEM_MAX_FILES) {
			return -KOS_ENOSPC;
		}
		f = &mem_files[mem_file_count++];
		memset(f, 0, sizeof(*f));
		strncpy(f->name, path, sizeof(f->name) - 1u);
		f->data = (uint8_t *)malloc(1u);
		f->data[0] = 0u;
		f->size = 0u;
		f->capacity = 1u;
	}
	slot = take_slot(ctx);
	if (slot == (struct mem_slot *)0) {
		return -KOS_EBUSY;
	}
	slot->file = (unsigned int)(f - mem_files);
	slot->cursor = 0u;
	++open_count;
	return 0;
}

static int mem_close(void *ctx)
{
	struct mem_slot *slot = find_slot(ctx);

	if (slot != (struct mem_slot *)0) {
		slot->file = MEM_MAX_FILES;
		slot->ctx = (void *)0;
	}
	return 0;
}

static int mem_size(void *ctx, uint32_t *out)
{
	struct mem_slot *slot = find_slot(ctx);

	if (slot == (struct mem_slot *)0) {
		return -KOS_EIO;
	}
	*out = mem_files[slot->file].size;
	return 0;
}

static int mem_read(void *ctx, void *dst, unsigned int len)
{
	struct mem_slot *slot = find_slot(ctx);
	struct mem_file *f;
	uint32_t available;

	if (slot == (struct mem_slot *)0) {
		return -KOS_EIO;
	}
	f = &mem_files[slot->file];
	if (fail_read_ord == read_count) {
		++read_count;
		return fail_read_code;
	}
	if (short_read_ord == read_count) {
		len /= 2u;
	}
	++read_count;
	available = f->size - slot->cursor;
	if (len > available) {
		len = available;
	}
	memcpy(dst, f->data + slot->cursor, len);
	slot->cursor += len;
	return (int)len;
}

static int mem_write(void *ctx, const void *src, unsigned int len)
{
	struct mem_slot *slot = find_slot(ctx);
	struct mem_file *f;
	uint32_t end;

	if (slot == (struct mem_slot *)0) {
		return -KOS_EIO;
	}
	f = &mem_files[slot->file];
	if (fail_write_ord == write_count) {
		++write_count;
		return fail_write_code;
	}
	++write_count;
	end = slot->cursor + len;
	if (end > f->capacity) {
		uint32_t cap = f->capacity;

		while (cap < end) {
			cap = (uint32_t)(cap + cap / 2u + 8u);
		}
		f->data = (uint8_t *)realloc(f->data, cap);
		f->capacity = cap;
	}
	memcpy(f->data + slot->cursor, src, len);
	if (end > f->size) {
		f->size = end;
	}
	slot->cursor = end;
	return (int)len;
}

static int mem_seek(void *ctx, uint32_t offset)
{
	struct mem_slot *slot = find_slot(ctx);

	if (slot == (struct mem_slot *)0) {
		return -KOS_EIO;
	}
	if (fail_seek_ord == seek_count) {
		++seek_count;
		return fail_seek_code;
	}
	++seek_count;
	slot->cursor = offset;
	return 0;
}

static int mem_sync(void *ctx)
{
	if (find_slot(ctx) == (struct mem_slot *)0) {
		return -KOS_EIO;
	}
	if (fail_sync_ord == sync_count) {
		++sync_count;
		return fail_sync_code;
	}
	++sync_count;
	return 0;
}

static int mem_truncate(void *ctx, uint32_t length)
{
	struct mem_slot *slot = find_slot(ctx);
	struct mem_file *f;

	if (slot == (struct mem_slot *)0) {
		return -KOS_EIO;
	}
	f = &mem_files[slot->file];
	if (refuse_growth != 0 && length > f->size) {
		return -KOS_EINVAL; /* the contract: truncate never grows */
	}
	if (length > f->capacity) {
		f->data = (uint8_t *)realloc(f->data, length);
		f->capacity = length;
	}
	if (length > f->size) {
		memset(f->data + f->size, 0, length - f->size);
	}
	f->size = length;
	return 0;
}

static int mem_rename(void *ctx, const char *from, const char *to)
{
	struct mem_file *f = find_file(from);

	(void)ctx;
	if (f == (struct mem_file *)0) {
		return -KOS_ENOENT;
	}
	strncpy(f->name, to, sizeof(f->name) - 1u);
	return 0;
}

static int mem_unlink(void *ctx, const char *path)
{
	struct mem_file *f = find_file(path);

	(void)ctx;
	if (f == (struct mem_file *)0) {
		return -KOS_ENOENT;
	}
	free(f->data);
	memmove(f, f + 1,
		(mem_file_count - 1u - (unsigned int)(f - mem_files)) *
			sizeof(*f));
	--mem_file_count;
	return 0;
}

static struct kres_io mem_adapter = {
	mem_open, mem_close, mem_size, mem_read, mem_write,
	mem_seek, mem_sync, mem_truncate, mem_rename, mem_unlink,
};

const struct kres_io *mem_io(void)
{
	return &mem_adapter;
}
