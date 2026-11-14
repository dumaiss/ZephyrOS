/* KRES host tests: the XMEM shim's objects (see kos.h beside it). */

#include <stdlib.h>
#include <string.h>

#include "kos.h"

#define SHIM_OBJECTS 32u
#define SHIM_MAX_OBJECT 0x8000u

static struct {
	uint8_t *data;
	unsigned int size;
	unsigned int generation;
} objects[SHIM_OBJECTS];

static unsigned long live_bytes;
static unsigned long peak_bytes;
static unsigned long limit_bytes;

static int lookup(unsigned int handle)
{
	unsigned int index = (handle & 0x1Fu);

	if (handle == 0u || objects[index].data == (uint8_t *)0 ||
	    (handle >> 5) != objects[index].generation) {
		return -1;
	}
	return (int)index;
}

int xalloc(unsigned int size, unsigned int flags)
{
	unsigned int i;

	(void)flags;
	if (size == 0u || size > SHIM_MAX_OBJECT) {
		return -KOS_EINVAL;
	}
	if (limit_bytes != 0u && live_bytes + size > limit_bytes) {
		return -KOS_ENOSPC;
	}
	for (i = 0u; i < SHIM_OBJECTS; ++i) {
		if (objects[i].data == (uint8_t *)0) {
			objects[i].data = (uint8_t *)malloc(size);
			memset(objects[i].data, 0xA5, size); /* undefined */
			objects[i].size = size;
			objects[i].generation =
				(objects[i].generation % 15u) + 1u;
			live_bytes += size;
			if (live_bytes > peak_bytes) {
				peak_bytes = live_bytes;
			}
			return (int)((objects[i].generation << 5) | i);
		}
	}
	return -KOS_ENOSPC;
}

int xfree(unsigned int handle)
{
	int index = lookup(handle);

	if (index < 0) {
		return -KOS_EINVAL;
	}
	live_bytes -= objects[index].size;
	free(objects[index].data);
	objects[index].data = (uint8_t *)0;
	return 0;
}

int xread(unsigned int handle, unsigned int offset, void *buffer,
	  unsigned int length)
{
	int index = lookup(handle);

	if (index < 0 || offset + length > objects[index].size) {
		return -KOS_EINVAL;
	}
	memcpy(buffer, objects[index].data + offset, length);
	return (int)length;
}

int xwrite(unsigned int handle, unsigned int offset, const void *buffer,
	   unsigned int length)
{
	int index = lookup(handle);

	if (index < 0 || offset + length > objects[index].size) {
		return -KOS_EINVAL;
	}
	memcpy(objects[index].data + offset, buffer, length);
	return (int)length;
}

unsigned int xmem_shim_objects(void)
{
	unsigned int i;
	unsigned int n = 0u;

	for (i = 0u; i < SHIM_OBJECTS; ++i) {
		n += objects[i].data != (uint8_t *)0;
	}
	return n;
}

unsigned long xmem_shim_bytes(void)
{
	return live_bytes;
}

unsigned long xmem_shim_peak(void)
{
	return peak_bytes;
}

void xmem_shim_limit(unsigned long bytes)
{
	limit_bytes = bytes;
}
