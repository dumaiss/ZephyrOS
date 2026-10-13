/* KonaOS filesystem outcome acceptance: the nested-exec child.
 *
 * SYS_EXEC starts it from /system/fs_child.exe with the tail the parent
 * passes, so this program proves three things the parent cannot:
 *
 *   * a child writes through its own standard handle;
 *   * the argument tail survives the exec (crt0's startup block and the
 *     split argv agree on it);
 *   * a process that *leaks* every handle it opened - a bound `/fs`
 *     instance, a file it created on the card and a recovery-store file -
 *     leaves no trace for the next program: the replay reads the driver's
 *     handle slots and the kernel's descriptor table after this program
 *     exits.
 *
 * Its exit status is the value the parent compares (KOS_EXIT_STATUS).
 *
 * Two tails are accepted.  `tail-42` is the fs image's
 * contract: the card file is /fs/CHILD.TMP.  `stage` runs it as the
 * hardware exerciser's nested exec, where every write stays inside the
 * /fs/HWTEST scratch directory the runner owns.  Both are checked
 * byte-exact, and everything else about the program is unchanged.
 */

#include "kos.h"

#define KOS_EXIT_STATUS 42

#define CHILD_TAIL "tail-42"
#define CHILD_STAGE "stage"
#define CHILD_FILE "/fs/CHILD.TMP"
#define CHILD_STAGE_FILE "/fs/HWTEST/CHILD.TMP"

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
	struct kos_startup *boot = startup();
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

	console_puts("fs-child: stdout ok\r\n");

	if (boot == (struct kos_startup *)0 || boot->version != 1u ||
	    boot->length != (unsigned char)tail_length(tail) ||
	    !same(boot->tail, tail)) {
		console_puts("fs-child: FAIL startup\r\n");
		ok = 0;
	}
	if (argc != 1 || argv == (char **)0 || !same(argv[0], tail)) {
		console_puts("fs-child: FAIL argv\r\n");
		ok = 0;
	}
	printf("fs-child: tail \"%s\" argc %u\r\n", boot->tail,
		   (unsigned int)argc);

	/* Handles, none of them closed on purpose.  A card that is absent
	 * or write-protected legitimately refuses the card file, so only the
	 * `/fs` handle and the store handle are then leaked. */
	bound = bind("/fs", 0u, &type);
	if (bound >= (int)KOS_HANDLE_FIRST_BOUND && type == KOS_TYPE_FS) {
		++leaked;
	} else if (bound != -KOS_ENODEV && bound != -KOS_EROFS) {
		printf("fs-child: FAIL bind %d\r\n", bound);
		ok = 0;
	}
	card_file = open(card_path,
			     (unsigned char)(KFS_O_WRITE | KFS_O_CREATE |
					     KFS_O_TRUNC));
	if (card_file >= (int)KOS_HANDLE_FIRST_BOUND) {
		++leaked;
	} else if (card_file == -KOS_ENODEV) {
		console_puts("fs-child: card absent\r\n");
	} else if (card_file == -KOS_EROFS) {
		console_puts("fs-child: card not writable\r\n");
	} else {
		printf("fs-child: FAIL create %d\r\n", card_file);
		ok = 0;
	}
	system_file = open("/system/drivers.man", KFS_O_READ);
	if (system_file >= (int)KOS_HANDLE_FIRST_BOUND) {
		++leaked;
	} else {
		printf("fs-child: FAIL system %d\r\n", system_file);
		ok = 0;
	}
	if (ok) {
		printf("fs-child: leaked %u handles\r\n",
			   (unsigned int)leaked);
	}

	console_puts(ok ? "fs-child: PASS\r\n" : "fs-child: FAIL\r\n");
	return ok ? KOS_EXIT_STATUS : 1;
}
