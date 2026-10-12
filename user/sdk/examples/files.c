/* KonaOS SDK example: open, read, list (work package 49c).
 *
 * The filesystem half of the SDK against the store: `open` binds a
 * program from `/system`, `read` streams bytes out of it, `list` pages a
 * namespace directory and `close` releases the handle.  The first
 * argument is the file to read, the second the directory to list; both
 * default to the store's own `/system`.
 *
 * A program reads the store through the VFS providers the image's store
 * carries; the FAT card is the same surface below `/fs`.
 */

#include "kos.h"

#define ENUM_PAGE 4
#define READ_CHUNK 16

static struct kos_ns_entry entries[ENUM_PAGE];
static char buffer[READ_CHUNK];

static int read_head(const char *path)
{
	int handle = open(path, KFS_O_READ);
	int count;

	if (handle < 0) {
		printf("files: open %s: %d\r\n", path, handle);
		return handle;
	}
	count = read((unsigned int)handle, buffer, sizeof(buffer));
	(void)close((unsigned int)handle);
	if (count < 0) {
		printf("files: read %s: %d\r\n", path, count);
		return count;
	}
	printf("files: read %d bytes of %s\r\n", count, path);
	return 0;
}

static int list_directory(const char *path)
{
	unsigned int cookie = 0u;
	int total = 0;

	for (;;) {
		int count = list(path, &cookie, entries, ENUM_PAGE);
		int index;

		if (count < 0) {
			printf("files: list %s: %d\r\n", path, count);
			return count;
		}
		if (count == 0) {
			break;
		}
		for (index = 0; index < count; ++index) {
			if (entries[index].flags & KOS_NS_ENTRY_CONTAINER) {
				printf("files: %s/%s/\r\n", path,
				       entries[index].name);
			} else {
				printf("files: %s/%s\r\n", path,
				       entries[index].name);
			}
			++total;
		}
	}
	printf("files: %d entries under %s\r\n", total, path);
	return 0;
}

int main(int argc, char **argv)
{
	const char *file = "/system/hello.exe";
	const char *directory = "/system";

	if (argc > 1 && argv[1] != (char *)0) {
		file = argv[1];
	}
	if (argc > 2 && argv[2] != (char *)0) {
		directory = argv[2];
	}
	if (read_head(file) < 0) {
		return 1;
	}
	if (list_directory(directory) < 0) {
		return 1;
	}
	console_puts("files: PASS\r\n");
	return 0;
}
