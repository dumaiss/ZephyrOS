/* kres: the host-side KRES tool.
 *
 * A single binary implementing the §9.2 command surface over the KRES
 * core with a stdio adapter.  Names are treated as CP850 and converted
 * at this boundary only; payloads are copied byte for byte.
 *
 * Build (host): cc -std=c99 -Wall -Wextra -Werror.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <errno.h>

#include "kos/kres.h"
#include "kos/errno.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>

/* ---------------------------------------------------------------- */
/* CP850 boundary conversion.                                        */
/* ---------------------------------------------------------------- */

/* The upper half of CP850, as Unicode code points. */
static const unsigned short cp850_upper[128] = {
	0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
	0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
	0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
	0x00FF, 0x00D6, 0x00DC, 0x00F8, 0x00A3, 0x00D8, 0x00D7, 0x0192,
	0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
	0x00BF, 0x00AE, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
	0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x00C1, 0x00C2, 0x00C0,
	0x00A9, 0x2563, 0x2551, 0x2557, 0x255D, 0x00A2, 0x00A5, 0x2510,
	0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x00E3, 0x00C3,
	0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x00A4,
	0x00F0, 0x00D0, 0x00CA, 0x00CB, 0x00C8, 0x0131, 0x00CD, 0x00CE,
	0x00CF, 0x2518, 0x250C, 0x2588, 0x2584, 0x00A6, 0x00CC, 0x2580,
	0x00D3, 0x00DF, 0x00D4, 0x00D2, 0x00F5, 0x00D5, 0x00B5, 0x00FE,
	0x00DE, 0x00DA, 0x00DB, 0x00D9, 0x00FD, 0x00DD, 0x00AF, 0x00B4,
	0x00AD, 0x00B1, 0x2017, 0x00BE, 0x00B6, 0x00A7, 0x00F7, 0x00B8,
	0x00B0, 0x00A8, 0x00B7, 0x00B9, 0x00B3, 0x00B2, 0x25A0, 0x00A0,
};

/* Encode one Unicode code point as UTF-8; returns bytes written. */
static int utf8_put(unsigned int cp, char *out)
{
	if (cp < 0x80u) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800u) {
		out[0] = (char)(0xC0u | (cp >> 6));
		out[1] = (char)(0x80u | (cp & 0x3Fu));
		return 2;
	}
	out[0] = (char)(0xE0u | (cp >> 12));
	out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
	out[2] = (char)(0x80u | (cp & 0x3Fu));
	return 3;
}

/* CP850 byte string to UTF-8; returns 0 on success. */
static int cp850_to_utf8(const char *in, size_t len, char *out,
			 size_t out_cap)
{
	size_t i;
	size_t n = 0u;

	for (i = 0u; i < len; ++i) {
		unsigned char b = (unsigned char)in[i];
		unsigned int cp = b < 0x80u ? (unsigned int)b
					   : (unsigned int)cp850_upper[b - 0x80u];
		char tmp[4];
		int w = utf8_put(cp, tmp);

		if (n + (size_t)w + 1u > out_cap) {
			return -1;
		}
		memcpy(out + n, tmp, (size_t)w);
		n += (size_t)w;
	}
	out[n] = '\0';
	return 0;
}

/* Decode one UTF-8 sequence; returns its length and the code point. */
static int utf8_get(const char *in, size_t len, unsigned int *cp)
{
	unsigned char b = (unsigned char)in[0];

	if (b < 0x80u) {
		*cp = (unsigned int)b;
		return 1;
	}
	if (b >= 0xC2u && b <= 0xDFu && len >= 2u &&
	    ((unsigned char)in[1] & 0xC0u) == 0x80u) {
		*cp = ((unsigned int)(b & 0x1Fu) << 6) |
		      (unsigned int)((unsigned char)in[1] & 0x3Fu);
		return 2;
	}
	if (b >= 0xE0u && b <= 0xEFu && len >= 3u &&
	    ((unsigned char)in[1] & 0xC0u) == 0x80u &&
	    ((unsigned char)in[2] & 0xC0u) == 0x80u) {
		*cp = ((unsigned int)(b & 0x0Fu) << 12) |
		      ((unsigned int)((unsigned char)in[1] & 0x3Fu) << 6) |
		      (unsigned int)((unsigned char)in[2] & 0x3Fu);
		return 3;
	}
	return -1;
}

/* UTF-8 string to CP850; returns 0 on success. */
static int utf8_to_cp850(const char *in, char *out, size_t out_cap)
{
	size_t i = 0u;
	size_t n = 0u;

	while (in[i] != '\0') {
		unsigned int cp;
		int w = utf8_get(in + i, strlen(in + i), &cp);

		if (w < 0) {
			return -1;
		}
		i += (size_t)w;
		if (cp < 0x80u) {
			if (n + 1u > out_cap) {
				return -1;
			}
			out[n++] = (char)cp;
			continue;
		}
		{
			int hit = -1;
			int k;

			for (k = 0; k < 128; ++k) {
				if (cp850_upper[k] == (unsigned short)cp) {
					hit = k;
					break;
				}
			}
			if (hit < 0 || n + 1u > out_cap) {
				return -1;
			}
			out[n++] = (char)(0x80 + hit);
		}
	}
	if (n == 0u) {
		return -1;
	}
	out[n] = '\0';
	return 0;
}

/* ---------------------------------------------------------------- */
/* Host stdio adapter for the KRES core.                             */
/* ---------------------------------------------------------------- */

/* The host stdio adapter holds up to two open files, keyed by the ctx
 * each container was opened with: compaction keeps the source and the
 * rebuilt container open at the same time (design §7.1). */

#define HOST_SLOTS 2u

static struct {
	void *ctx;
	FILE *file;
} host_slots[HOST_SLOTS];

static int host_slot_of(void *ctx)
{
	unsigned int i;

	for (i = 0u; i < HOST_SLOTS; ++i) {
		if (host_slots[i].ctx == ctx && host_slots[i].file !=
						   (FILE *)0) {
			return (int)i;
		}
	}
	return -1;
}

static FILE *host_file_of(void *ctx)
{
	int slot = host_slot_of(ctx);

	return slot < 0 ? (FILE *)0 : host_slots[slot].file;
}

static int host_open(void *ctx, const char *path, unsigned int flags)
{
	const char *mode;
	unsigned int i;
	int slot = host_slot_of(ctx);

	if (slot < 0) {
		for (i = 0u; i < HOST_SLOTS; ++i) {
			if (host_slots[i].file == (FILE *)0) {
				slot = (int)i;
				break;
			}
		}
	}
	if (slot < 0) {
		return -KOS_EBUSY;
	}
	mode = (flags & KRES_OPEN_CREATE) != 0u ? "w+b" : "r+b";
	host_slots[slot].file = fopen(path, mode);
	if (host_slots[slot].file == (FILE *)0) {
		host_slots[slot].ctx = (void *)0;
		return -KOS_ENOENT;
	}
	host_slots[slot].ctx = ctx;
	return 0;
}

static int host_close(void *ctx)
{
	int slot = host_slot_of(ctx);

	if (slot < 0) {
		return 0;
	}
	if (fclose(host_slots[slot].file) != 0) {
		host_slots[slot].file = (FILE *)0;
		host_slots[slot].ctx = (void *)0;
		return -KOS_EIO;
	}
	host_slots[slot].file = (FILE *)0;
	host_slots[slot].ctx = (void *)0;
	return 0;
}

static int host_size(void *ctx, uint32_t *size)
{
	FILE *file = host_file_of(ctx);
	long end;

	if (file == (FILE *)0) {
		return -KOS_EIO;
	}
	if (fseek(file, 0L, SEEK_END) != 0) {
		return -KOS_EIO;
	}
	end = ftell(file);
	if (end < 0L) {
		return -KOS_EIO;
	}
	*size = (uint32_t)end;
	return 0;
}

static int host_read(void *ctx, void *dst, unsigned int len)
{
	FILE *file = host_file_of(ctx);

	if (file == (FILE *)0) {
		return -KOS_EIO;
	}
	return (int)fread(dst, 1u, (size_t)len, file);
}

static int host_write(void *ctx, const void *src, unsigned int len)
{
	FILE *file = host_file_of(ctx);

	if (file == (FILE *)0) {
		return -KOS_EIO;
	}
	if (fwrite(src, 1u, (size_t)len, file) != (size_t)len) {
		return -KOS_EIO;
	}
	return (int)len;
}

static int host_seek(void *ctx, uint32_t offset)
{
	FILE *file = host_file_of(ctx);

	if (file == (FILE *)0) {
		return -KOS_EIO;
	}
	if (offset > 0x7FFFFFFFu) {
		return -KOS_EINVAL;
	}
	return fseek(file, (long)offset, SEEK_SET) == 0 ? 0 : -KOS_EIO;
}

static int host_sync(void *ctx)
{
	FILE *file = host_file_of(ctx);

	if (file == (FILE *)0) {
		return -KOS_EIO;
	}
	return fflush(file) == 0 ? 0 : -KOS_EIO;
}

static int host_truncate(void *ctx, uint32_t size)
{
	FILE *file = host_file_of(ctx);
	long end;

	if (file == (FILE *)0) {
		return -KOS_EIO;
	}
	if (fflush(file) != 0) {
		return -KOS_EIO;
	}
	if (fseek(file, 0L, SEEK_END) != 0) {
		return -KOS_EIO;
	}
	end = ftell(file);
	if (end < 0L) {
		return -KOS_EIO;
	}
	/* The contract: truncate never grows a file (growth writes zero
	 * records through write). */
	if ((uint32_t)end < size) {
		return -KOS_EINVAL;
	}
	if (ftruncate(fileno(file), (off_t)size) != 0) {
		return -KOS_EIO;
	}
	return 0;
}

static int host_rename(void *ctx, const char *from, const char *to)
{
	(void)ctx;
	return rename(from, to) == 0 ? 0 : -KOS_EIO;
}

static int host_unlink(void *ctx, const char *path)
{
	(void)ctx;
	return remove(path) == 0 ? 0 : -KOS_ENOENT;
}

static struct kres_io host_io = {
	host_open, host_close, host_size, host_read, host_write,
	host_seek, host_sync, host_truncate, host_rename, host_unlink,
};

/* ---------------------------------------------------------------- */
/* Export name safety and collisions.                                */
/* ---------------------------------------------------------------- */

static int unsafe_name(const char *name, size_t len)
{
	size_t i;

	if (len == 0u) {
		return 1;
	}
	if (name[0] == '/' || name[0] == '\\') {
		return 1;
	}
	for (i = 0u; i < len; ++i) {
		if (name[i] == ':' || name[i] == '\0') {
			return 1;
		}
		if (i + 1u < len && name[i] == '.' && name[i + 1] == '.' &&
		    (i == 0u || name[i - 1] == '/' || name[i - 1] == '\\')) {
			return 1; /* ".." component */
		}
	}
	return 0;
}

/* The paths already created by this unpack, for the case-insensitive
 * collision refusal. */
static char unpacked_paths[KRES_RESOURCE_MAX][512];
static unsigned int unpacked_count;

static int path_clashes(const char *path)
{
	unsigned int i;

	for (i = 0u; i < unpacked_count; ++i) {
		if (strcasecmp(unpacked_paths[i], path) == 0) {
			return 1;
		}
	}
	return 0;
}

/* Create the parent directories of `path` (they may nest). */
static int mkdirs_for(const char *path)
{
	char tmp[512];
	size_t len = strlen(path);
	size_t i;

	if (len + 1u > sizeof(tmp)) {
		return -1;
	}
	memcpy(tmp, path, len + 1u);
	for (i = 1u; i < len; ++i) {
		if (tmp[i] != '/') {
			continue;
		}
		tmp[i] = '\0';
		if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
			return -1;
		}
		tmp[i] = '/';
	}
	return 0;
}

/* ---------------------------------------------------------------- */
/* Commands.                                                         */
/* ---------------------------------------------------------------- */

static void die(const char *what)
{
	fprintf(stderr, "kres: %s\n", what);
	exit(1);
}

static int cmd_create(int argc, char **argv)
{
	int result;

	if (argc != 1) {
		return 1;
	}
	result = kres_create_container(argv[0]);
	if (result < 0) {
		fprintf(stderr, "kres: create %s failed (%d)\n", argv[0],
			result);
		return 1;
	}
	return 0;
}

static int cmd_list(int argc, char **argv)
{
	kres_container c;
	struct kres_iter it;
	struct kres_info info;
	char name[KRES_NAME_MAX * 3u + 1u];
	int result;

	if (argc != 1) {
		return 1;
	}
	result = kres_open(&c, argv[0], KRES_OPEN_READ);
	if (result < 0) {
		fprintf(stderr, "kres: open %s failed (%d)\n", argv[0],
			result);
		return 1;
	}
	result = kres_iter_begin(&c, &it);
	if (result < 0) {
		fprintf(stderr, "kres: iter failed (%d)\n", result);
		(void)kres_close(&c);
		return 1;
	}
	while (kres_iter_next(&it, &info) > 0) {
		if (cp850_to_utf8((const char *)info.name, info.name_length,
				  name, sizeof(name)) != 0) {
			(void)kres_close(&c);
			die("name conversion failed");
		}
		printf("%-28s %6u %6u\n", name, (unsigned int)info.used_bytes,
		       (unsigned int)info.allocated_records);
	}
	(void)kres_close(&c);
	return 0;
}

static int cmd_show(int argc, char **argv)
{
	kres_container c;
	struct kres_info info;
	char name[KRES_NAME_MAX + 1u];
	char display[KRES_NAME_MAX * 3u + 1u];
	int result;

	if (argc != 2) {
		return 1;
	}
	result = kres_open(&c, argv[0], KRES_OPEN_READ);
	if (result < 0) {
		fprintf(stderr, "kres: open %s failed (%d)\n", argv[0],
			result);
		return 1;
	}
	if (utf8_to_cp850(argv[1], name, sizeof(name)) != 0) {
		(void)kres_close(&c);
		die("name not representable in CP850");
	}
	result = kres_stat(&c, name, &info);
	if (result < 0) {
		fprintf(stderr, "kres: stat %s failed (%d)\n", argv[1],
			result);
		(void)kres_close(&c);
		return 1;
	}
	cp850_to_utf8((const char *)info.name, info.name_length, display,
		      sizeof(display));
	printf("name      %s\n", display);
	printf("used      %u\n", (unsigned int)info.used_bytes);
	printf("allocated %u\n", (unsigned int)info.allocated_records);
	(void)kres_close(&c);
	return 0;
}

static int cmd_import(int argc, char **argv)
{
	static uint8_t buf[KRES_RECORD_SIZE];
	kres_container c;
	kres_resource r;
	char name[KRES_NAME_MAX + 1u];
	FILE *in;
	int result;

	if (argc != 3) {
		return 1;
	}
	result = kres_open(&c, argv[0], KRES_OPEN_READ | KRES_OPEN_WRITE);
	if (result < 0) {
		fprintf(stderr, "kres: open %s failed (%d)\n", argv[0],
			result);
		return 1;
	}
	if (utf8_to_cp850(argv[2], name, sizeof(name)) != 0) {
		(void)kres_close(&c);
		die("name not representable in CP850");
	}
	result = kres_create(&c, name, 1u, &r);
	if (result < 0) {
		fprintf(stderr, "kres: create %s failed (%d)\n", argv[2],
			result);
		(void)kres_close(&c);
		return 1;
	}
	in = fopen(argv[1], "rb");
	if (in == (FILE *)0) {
		(void)kres_delete(&c, name);
		(void)kres_close(&c);
		die("cannot read the import source");
	}
	for (;;) {
		size_t got = fread(buf, 1u, sizeof(buf), in);

		if (got == 0u) {
			break;
		}
		if (kres_write(&r, buf, (uint16_t)got) < 0) {
			(void)fclose(in);
			(void)kres_delete(&c, name);
			(void)kres_close(&c);
			die("write failed");
		}
	}
	(void)fclose(in);
	(void)kres_rclose(&r);
	(void)kres_close(&c);
	return 0;
}

static int cmd_extract(int argc, char **argv)
{
	kres_container c;
	char name[KRES_NAME_MAX + 1u];
	int result;

	if (argc != 3) {
		return 1;
	}
	result = kres_open(&c, argv[0], KRES_OPEN_READ);
	if (result < 0) {
		fprintf(stderr, "kres: open %s failed (%d)\n", argv[0],
			result);
		return 1;
	}
	if (utf8_to_cp850(argv[1], name, sizeof(name)) != 0) {
		(void)kres_close(&c);
		die("name not representable in CP850");
	}
	result = kres_extract(&c, name, argv[2]);
	if (result < 0) {
		fprintf(stderr, "kres: extract failed (%d)\n", result);
		(void)kres_close(&c);
		return 1;
	}
	(void)kres_close(&c);
	return 0;
}

static int cmd_unpack(int argc, char **argv)
{
	kres_container c;
	struct kres_iter it;
	struct kres_info info;
	char converted[KRES_NAME_MAX * 3u + 1u];
	char path[512];
	int result;

	if (argc != 2) {
		return 1;
	}
	result = kres_open(&c, argv[0], KRES_OPEN_READ);
	if (result < 0) {
		fprintf(stderr, "kres: open %s failed (%d)\n", argv[0],
			result);
		return 1;
	}
	unpacked_count = 0u;
	result = kres_iter_begin(&c, &it);
	if (result < 0) {
		(void)kres_close(&c);
		die("iter begin failed");
	}
	while (kres_iter_next(&it, &info) > 0) {
		if (unsafe_name((const char *)info.name, info.name_length) !=
		    0) {
			fprintf(stderr, "kres: refusing unsafe name\n");
			(void)kres_close(&c);
			return 1;
		}
		if (cp850_to_utf8((const char *)info.name, info.name_length,
				  converted, sizeof(converted)) != 0) {
			(void)kres_close(&c);
			die("name conversion failed");
		}
		if (snprintf(path, sizeof(path), "%s/%s", argv[1], converted) >=
		    (int)sizeof(path)) {
			(void)kres_close(&c);
			die("export path too long");
		}
		if (path_clashes(path) != 0 || access(path, F_OK) == 0) {
			fprintf(stderr, "kres: refusing export collision\n");
			(void)kres_close(&c);
			return 1;
		}
		if (mkdirs_for(path) != 0) {
			(void)kres_close(&c);
			die("cannot create the export directories");
		}
		result = kres_extract(&c, (const char *)info.name, path);
		if (result < 0) {
			fprintf(stderr, "kres: extract failed (%d)\n", result);
			(void)kres_close(&c);
			return 1;
		}
		if (unpacked_count >= KRES_RESOURCE_MAX ||
		    strlen(path) + 1u > sizeof(unpacked_paths[0])) {
			(void)kres_close(&c);
			die("too many export paths");
		}
		memcpy(unpacked_paths[unpacked_count], path, strlen(path) + 1u);
		unpacked_count++;
	}
	(void)kres_close(&c);
	return 0;
}

static int cmd_verify(int argc, char **argv)
{
	int result;

	if (argc != 1) {
		return 1;
	}
	result = kres_verify(argv[0]);
	if (result < 0) {
		fprintf(stderr, "kres: verify failed (%d)\n", result);
		return 1;
	}
	printf("%s: ok\n", argv[0]);
	return 0;
}

static int cmd_compact(int argc, char **argv)
{
	int tight = 0;
	int result;

	if (argc >= 2 && strcmp(argv[argc - 1], "--tight") == 0) {
		tight = 1;
		--argc;
	}
	if (argc != 1) {
		return 1;
	}
	result = kres_compact(argv[0], tight != 0 ? KRES_TIGHT : 0u);
	if (result < 0) {
		fprintf(stderr, "kres: compact failed (%d)\n", result);
		return 1;
	}
	return 0;
}

static int cmd_seal(int argc, char **argv)
{
	static uint8_t buf[KRES_RECORD_SIZE];
	kres_container c;
	struct kres_iter it;
	struct kres_info info;
	int result;

	if (argc != 2) {
		return 1;
	}
	result = kres_open(&c, argv[0], KRES_OPEN_READ);
	if (result < 0) {
		fprintf(stderr, "kres: open %s failed (%d)\n", argv[0],
			result);
		return 1;
	}
	result = kres_build_begin(argv[1], (uint8_t)c.resource_count);
	if (result < 0) {
		(void)kres_close(&c);
		die("build begin failed");
	}
	result = kres_iter_begin(&c, &it);
	if (result < 0) {
		goto seal_fail;
	}
	while (kres_iter_next(&it, &info) > 0) {
		kres_resource r;
		int got;

		result = kres_build_add((const char *)info.name);
		if (result < 0) {
			goto seal_fail;
		}
		result = kres_get(&c, (const char *)info.name, &r,
				  KRES_OPEN_READ);
		if (result < 0) {
			goto seal_fail;
		}
		for (;;) {
			got = kres_read(&r, buf, sizeof(buf));
			if (got < 0) {
				(void)kres_rclose(&r);
				result = got;
				goto seal_fail;
			}
			if (got == 0) {
				break;
			}
			result = kres_build_write(buf, (uint16_t)got);
			if (result < 0) {
				(void)kres_rclose(&r);
				goto seal_fail;
			}
		}
		(void)kres_rclose(&r);
	}
	(void)kres_close(&c);
	result = kres_build_end();
	if (result < 0) {
		fprintf(stderr, "kres: seal failed (%d)\n", result);
		return 1;
	}
	return 0;

seal_fail:
	(void)kres_build_abort();
	(void)kres_close(&c);
	fprintf(stderr, "kres: seal failed (%d)\n", result);
	return 1;
}

static int pack_walk_files(const char *dir, const char *prefix,
			   uint16_t *count, int adding)
{
	struct dirent *de;
	DIR *d = opendir(dir);
	char full[512];
	char joined[512];

	if (d == (DIR *)0) {
		return -1;
	}
	while ((de = readdir(d)) != (struct dirent *)0) {
		struct stat st;
		int result;

		if (strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0) {
			continue;
		}
		if (snprintf(full, sizeof(full), "%s/%s", dir, de->d_name) >=
		    (int)sizeof(full)) {
			(void)closedir(d);
			return -1;
		}
		if (snprintf(joined, sizeof(joined), "%s%s", prefix,
			     de->d_name) >= (int)sizeof(joined)) {
			(void)closedir(d);
			return -1;
		}
		if (stat(full, &st) != 0) {
			(void)closedir(d);
			return -1;
		}
		if (S_ISDIR(st.st_mode)) {
			char deeper[512];

			if (snprintf(deeper, sizeof(deeper), "%s/", joined) >=
			    (int)sizeof(deeper)) {
				(void)closedir(d);
				return -1;
			}
			result = pack_walk_files(full, deeper, count, adding);
			if (result != 0) {
				(void)closedir(d);
				return result;
			}
			continue;
		}
		if (!S_ISREG(st.st_mode)) {
			continue;
		}
		if (unsafe_name(joined, strlen(joined)) != 0) {
			fprintf(stderr, "kres: refusing unsafe name %s\n",
				joined);
			(void)closedir(d);
			return -1;
		}
		if (adding != 0) {
			static uint8_t buf[KRES_RECORD_SIZE];
			char name[KRES_NAME_MAX + 1u];
			FILE *in;

			if (utf8_to_cp850(joined, name, sizeof(name)) != 0) {
				fprintf(stderr,
					"kres: name not CP850: %s\n", joined);
				(void)closedir(d);
				return -1;
			}
			result = kres_build_add(name);
			if (result < 0) {
				(void)closedir(d);
				return result;
			}
			in = fopen(full, "rb");
			if (in == (FILE *)0) {
				(void)closedir(d);
				return -1;
			}
			for (;;) {
				size_t got = fread(buf, 1u, sizeof(buf), in);

				if (got == 0u) {
					break;
				}
				result = kres_build_write(buf, (uint16_t)got);
				if (result < 0) {
					(void)fclose(in);
					(void)closedir(d);
					return result;
				}
			}
			(void)fclose(in);
		}
		if (count != (uint16_t *)0) {
			if (*count >= KRES_RESOURCE_MAX) {
				fprintf(stderr,
					"kres: more than 32 resources\n");
				(void)closedir(d);
				return -1;
			}
			(*count)++;
		}
	}
	(void)closedir(d);
	return 0;
}

static int cmd_pack(int argc, char **argv)
{
	uint16_t count = 0u;
	int result;

	if (argc != 2) {
		return 1;
	}
	result = pack_walk_files(argv[0], "", &count, 0);
	if (result != 0) {
		fprintf(stderr, "kres: cannot scan %s\n", argv[0]);
		return 1;
	}
	result = kres_build_begin(argv[1], (uint8_t)count);
	if (result < 0) {
		fprintf(stderr, "kres: build begin failed (%d)\n", result);
		return 1;
	}
	result = pack_walk_files(argv[0], "", (uint16_t *)0, 1);
	if (result != 0) {
		(void)kres_build_abort();
		fprintf(stderr, "kres: pack failed (%d)\n", result);
		return 1;
	}
	result = kres_build_end();
	if (result < 0) {
		fprintf(stderr, "kres: pack failed (%d)\n", result);
		return 1;
	}
	return 0;
}

static int usage(void)
{
	fprintf(stderr,
		"usage:\n"
		"  kres create  PROJECT.KPR\n"
		"  kres list    PROJECT.KPR\n"
		"  kres show    PROJECT.KPR NAME\n"
		"  kres import  PROJECT.KPR HOSTPATH NAME\n"
		"  kres extract PROJECT.KPR NAME HOSTPATH\n"
		"  kres unpack  PROJECT.KPR DIR\n"
		"  kres pack    DIR PROJECT.KPR\n"
		"  kres verify  PROJECT.KPR\n"
		"  kres compact PROJECT.KPR [--tight]\n"
		"  kres seal    PROJECT.KPR BUNDLE.KPR\n");
	return 1;
}

int main(int argc, char **argv)
{
	static uint8_t io_ctx;

	if (argc < 2) {
		return usage();
	}
	kres_install_mut(&host_io, &kres_cache_mem, &io_ctx);
	if (strcmp(argv[1], "create") == 0) {
		return cmd_create(argc - 2, argv + 2);
	}
	if (strcmp(argv[1], "list") == 0) {
		return cmd_list(argc - 2, argv + 2);
	}
	if (strcmp(argv[1], "show") == 0) {
		return cmd_show(argc - 2, argv + 2);
	}
	if (strcmp(argv[1], "import") == 0) {
		return cmd_import(argc - 2, argv + 2);
	}
	if (strcmp(argv[1], "extract") == 0) {
		return cmd_extract(argc - 2, argv + 2);
	}
	if (strcmp(argv[1], "unpack") == 0) {
		return cmd_unpack(argc - 2, argv + 2);
	}
	if (strcmp(argv[1], "pack") == 0) {
		return cmd_pack(argc - 2, argv + 2);
	}
	if (strcmp(argv[1], "verify") == 0) {
		return cmd_verify(argc - 2, argv + 2);
	}
	if (strcmp(argv[1], "compact") == 0) {
		return cmd_compact(argc - 2, argv + 2);
	}
	if (strcmp(argv[1], "seal") == 0) {
		return cmd_seal(argc - 2, argv + 2);
	}
	return usage();
}
