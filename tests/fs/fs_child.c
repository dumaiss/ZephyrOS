/* ZephyrOS filesystem outcome acceptance: the nested-exec child.
 *
 * SYS_EXEC starts it from /SYSTEM/fs_child.zex with the tail the parent
 * passes, so this program proves three things the parent cannot:
 *
 *   * a child writes through its own standard handle;
 *   * the argument tail survives the exec (crt0's startup block and the
 *     split argv agree on it);
 *   * a process that *leaks* every handle it opened - a bound `/FS`
 *     instance, a file it created on the card and a recovery-store file -
 *     leaves no trace for the next program: the replay reads the driver's
 *     handle slots and the kernel's descriptor table after this program
 *     exits.
 *
 * Its exit status is the value the parent compares (ZOS_EXIT_STATUS).
 *
 * Two tails are accepted (work package 45b).  `tail-42` is the fs image's
 * contract: the card file is /FS/CHILD.TMP.  `stage` runs it as the
 * hardware exerciser's nested exec, where every write stays inside the
 * /FS/HWTEST scratch directory the runner owns.  Both are checked
 * byte-exact, and everything else about the program is unchanged.
 */

#include "zos.h"

#define ZOS_EXIT_STATUS 42

#define CHILD_TAIL "tail-42"
#define CHILD_STAGE "stage"
#define CHILD_FILE "/FS/CHILD.TMP"
#define CHILD_STAGE_FILE "/FS/HWTEST/CHILD.TMP"

static int same(const char *a, const char *b)
{
	while (*a == *b) {
		if (*a == '\0') {
			return 1;
		}
		++a;
		++b;
	}
	return 0;
}

static unsigned int tail_length(const char *text)
{
	unsigned int length = 0u;

	while (text[length] != '\0') {
		++length;
	}
	return length;
}

int main(int argc, char **argv)
{
	struct zos_startup *boot = zos_args();
	const char *tail = CHILD_TAIL;
	const char *card_path = CHILD_FILE;
	unsigned char type = 0u;
	int card_file;
	int system_file;
	int bound;
	unsigned int leaked = 0u;
	int ok = 1;

	/* The stage tail moves the card file into the exerciser's scratch
	 * directory; nothing else differs. */
	if (argc == 1 && argv != (char **)0 && same(argv[0], CHILD_STAGE)) {
		tail = CHILD_STAGE;
		card_path = CHILD_STAGE_FILE;
	}

	zos_puts("fs-child: stdout ok\r\n");

	if (boot == (struct zos_startup *)0 || boot->version != 1u ||
	    boot->length != (unsigned char)tail_length(tail) ||
	    !same(boot->tail, tail)) {
		zos_puts("fs-child: FAIL startup\r\n");
		ok = 0;
	}
	if (argc != 1 || argv == (char **)0 || !same(argv[0], tail)) {
		zos_puts("fs-child: FAIL argv\r\n");
		ok = 0;
	}
	zos_printf("fs-child: tail \"%s\" argc %u\r\n", boot->tail,
		   (unsigned int)argc);

	/* Handles, none of them closed on purpose.  A card that is absent
	 * or write-protected legitimately refuses the card file, so only the
	 * `/FS` handle and the store handle are then leaked. */
	bound = zos_ipc_bind("/FS", 0u, &type);
	if (bound >= (int)ZOS_HANDLE_FIRST_BOUND && type == ZOS_TYPE_FS) {
		++leaked;
	} else if (bound != -ZOS_ENODEV && bound != -ZOS_EROFS) {
		zos_printf("fs-child: FAIL bind %d\r\n", bound);
		ok = 0;
	}
	card_file = zos_open(card_path,
			     (unsigned char)(ZFS_O_WRITE | ZFS_O_CREATE |
					     ZFS_O_TRUNC));
	if (card_file >= (int)ZOS_HANDLE_FIRST_BOUND) {
		++leaked;
	} else if (card_file == -ZOS_ENODEV) {
		zos_puts("fs-child: card absent\r\n");
	} else if (card_file == -ZOS_EROFS) {
		zos_puts("fs-child: card not writable\r\n");
	} else {
		zos_printf("fs-child: FAIL create %d\r\n", card_file);
		ok = 0;
	}
	system_file = zos_open("/SYSTEM/services.man", ZFS_O_READ);
	if (system_file >= (int)ZOS_HANDLE_FIRST_BOUND) {
		++leaked;
	} else {
		zos_printf("fs-child: FAIL system %d\r\n", system_file);
		ok = 0;
	}
	if (ok) {
		zos_printf("fs-child: leaked %u handles\r\n",
			   (unsigned int)leaked);
	}

	zos_puts(ok ? "fs-child: PASS\r\n" : "fs-child: FAIL\r\n");
	return ok ? ZOS_EXIT_STATUS : 1;
}
