/* KonaOS libkos: `xload`, the whole-file loader.
 *
 * A program picks a file at run time and wants its bytes as an object it
 * can copy from, or map.  The size comes from `stat` (nothing is guessed
 * from reads), the object is one `xalloc`, and the bytes travel once:
 *
 *   - a mappable object is read straight into the open aperture: the
 *     provider writes the file into the far bank (the buffer-identity
 *     rule, 4.9), so no byte lands in the program's own window;
 *   - a plain object has no address, so it is filled through the 512-byte
 *     SDK buffer and `xwrite`.
 *
 * The limit is checked before anything is allocated - a file over it is
 * -KOS_ENOSPC, and a program that must have such a file streams it in
 * pieces instead.  Every failure releases the object and the file handle:
 * a failed load leaves no trace.
 */

#include "kos.h"

/* The largest file `xload` takes, and the chunk the plain path moves. */
#define XLOAD_PLAIN_MAX    0x8000u /* 32 KiB: a whole object      */
#define XLOAD_MAPPABLE_MAX 0x4000u /* 16 KiB: the aperture slice  */
#define XLOAD_CHUNK        512u

/* Static, not on the stack: a program's default stack is 256 bytes. */
static unsigned char xload_buffer[XLOAD_CHUNK];

static int xload_fill_mappable(int object, int file, unsigned int size)
{
	unsigned int address = 0u;
	unsigned char saved;
	int token;
	int count;

	token = xmap((unsigned int)object, &address);
	if (token < 0) {
		return token;
	}
	saved = aperture_open((unsigned char)token);
	count = read((unsigned int)file, (void *)address, size);
	aperture_close(saved);
	if (count < 0) {
		return count;
	}
	if ((unsigned int)count != size) {
		return -KOS_EINVAL; /* the file changed under us */
	}
	return 0;
}

static int xload_fill_plain(int object, int file, unsigned int size)
{
	unsigned int offset = 0u;

	while (offset < size) {
		unsigned int chunk = size - offset;
		int count;
		int written;

		if (chunk > XLOAD_CHUNK) {
			chunk = XLOAD_CHUNK;
		}
		count = read((unsigned int)file, xload_buffer, chunk);
		if (count <= 0) {
			return (count < 0) ? count : -KOS_EINVAL;
		}
		written = xwrite((unsigned int)object, offset, xload_buffer,
				 (unsigned int)count);
		if (written != count) {
			return (written < 0) ? written : -KOS_EINVAL;
		}
		offset += (unsigned int)count;
	}
	return 0;
}

int xload(const char *path, unsigned int flags)
{
	struct kfs_stat info;
	unsigned int limit;
	int file;
	int object;
	int result;

	if ((flags & ~KOS_XMEM_FLAG_MAPPABLE) != 0u) {
		return -KOS_EINVAL;
	}
	limit = (flags & KOS_XMEM_FLAG_MAPPABLE) != 0u
		? XLOAD_MAPPABLE_MAX : XLOAD_PLAIN_MAX;
	result = stat(path, &info);
	if (result != 0) {
		return result;
	}
	if (info.size == 0ul) {
		return -KOS_EINVAL;
	}
	if (info.size > (uint32_t)limit) {
		return -KOS_ENOSPC;
	}
	object = xalloc((unsigned int)info.size, flags);
	if (object < 0) {
		return object;
	}
	file = open(path, KFS_O_READ);
	if (file < 0) {
		(void)xfree((unsigned int)object);
		return file;
	}
	if ((flags & KOS_XMEM_FLAG_MAPPABLE) != 0u) {
		result = xload_fill_mappable(object, file,
					     (unsigned int)info.size);
	} else {
		result = xload_fill_plain(object, file,
					  (unsigned int)info.size);
	}
	(void)close((unsigned int)file);
	if (result != 0) {
		(void)xfree((unsigned int)object);
		return result;
	}
	return object;
}
