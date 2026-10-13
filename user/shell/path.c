/* KonaOS native shell: path presentation and translation.
 *
 * Shell syntax is the user's view, and the VFS never aliases: /fs is
 * shown as /, any other absolute path gains /fs in front (so /GAMES is
 * /fs/GAMES), and // names the real namespace root.  Every path the
 * shell hands the namespace is built here, so the mapping is one place
 * and the shell's children (which see the real paths) always agree with
 * what the user typed.
 */

#include "shell.h"

static char path_fold(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* 1 when the first component of the absolute `path` is `name`, matched
 * case-insensitively as the whole component (so /systemx is not
 * /system). */
static unsigned char path_component_is(const char *path, const char *name)
{
	unsigned int index = 0;

	while (name[index] != '\0') {
		if (path_fold(path[1 + index]) != name[index]) {
			return 0;
		}
		++index;
	}
	return (path[1 + index] == '\0' || path[1 + index] == '/') ? 1u : 0u;
}

/* The reserved root a VFS path sits under, as its name, or NULL: `//x`
 * names the real root and a relative path names none. */
const char *shell_path_root(const char *path)
{
	if (path[0] != '/') {
		return (const char *)0;
	}
	if (path_component_is(path, "fs")) {
		return "fs";
	}
	if (path_component_is(path, "system")) {
		return "system";
	}
	if (path_component_is(path, "dev")) {
		return "dev";
	}
	return (const char *)0;
}

/* 1 for the reserved roots /fs, /system and /dev. */
static unsigned char path_reserved_root(const char *path)
{
	return (unsigned char)(shell_path_root(path) != (const char *)0);
}

int shell_path_translate(const char *path, char *out, unsigned int cap)
{
	const char *src = path;
	const char *prefix = "";
	unsigned int needed;

	if (path[0] == '/') {
		if (path[1] == '/') {
			/* // is the displayed VFS root and //x is /x. */
			src = path + 1;
		} else if (path[1] == '\0') {
			/* / is the display root, which lives at /fs. */
			src = "";
			prefix = "/fs";
		} else if (!path_reserved_root(path)) {
			prefix = "/fs";
		}
	}
	needed = strlen(prefix) + strlen(src);
	if (needed + 1u > cap) {
		return -KOS_EINVAL;
	}
	(void)strlcpy(out, prefix, cap);
	(void)strlcat(out, src, cap);
	return (int)needed;
}

int shell_path_display(const char *path, char *out, unsigned int cap)
{
	const char *src = path;
	const char *prefix = "";
	unsigned int needed;

	if (path[0] == '/') {
		if (path[1] == '\0') {
			src = "//";
		} else if (path_component_is(path, "fs")) {
			if (path[3] == '\0') {
				src = "/";
			} else {
				prefix = "/";
				src = path + 4;
			}
		}
	}
	needed = strlen(prefix) + strlen(src);
	if (needed + 1u > cap) {
		return -KOS_EINVAL;
	}
	(void)strlcpy(out, prefix, cap);
	(void)strlcat(out, src, cap);
	return (int)needed;
}
