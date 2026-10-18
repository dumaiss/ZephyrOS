/* KonaOS TUI SDK outcome program: the foundation rows.
 *
 * Built with `LIBS=tui` as build/user/tui_test.exe and carried on a card by
 * the replay tests.  The foundation mode initializes the library, checks
 * the logical geometry and transaction nesting, writes one marker through
 * ordinary console I/O, shuts down, and reports PASS.
 */

#include "kos.h"
#include "tui.h"

static int foundation(void)
{
	struct tui_size size;
	int result;

	result = tui_init();
	if (result < 0) {
		printf("tui: FAIL init %d\r\n", result);
		return 1;
	}
	result = tui_init();
	if (result != -KOS_EINVAL) {
		printf("tui: FAIL reinit %d\r\n", result);
		tui_shutdown();
		return 1;
	}
	result = tui_get_size(&size);
	if (result < 0) {
		printf("tui: FAIL size %d\r\n", result);
		tui_shutdown();
		return 1;
	}
	if (size.cols != 85u || size.rows != 26u) {
		printf("tui: FAIL geometry %u %u\r\n",
		       (unsigned int)size.cols, (unsigned int)size.rows);
		tui_shutdown();
		return 1;
	}
	result = tui_get_size((struct tui_size *)0);
	if (result != -KOS_EINVAL) {
		printf("tui: FAIL null size %d\r\n", result);
		tui_shutdown();
		return 1;
	}

	tui_begin();
	tui_begin();
	result = tui_end();
	if (result != 0) {
		printf("tui: FAIL inner end %d\r\n", result);
		tui_shutdown();
		return 1;
	}
	result = tui_end();
	if (result != 0) {
		printf("tui: FAIL outer end %d\r\n", result);
		tui_shutdown();
		return 1;
	}
	result = tui_end();
	if (result != -KOS_EINVAL) {
		printf("tui: FAIL unmatched end %d\r\n", result);
		tui_shutdown();
		return 1;
	}
	result = tui_flush();
	if (result != 0) {
		printf("tui: FAIL flush %d\r\n", result);
		tui_shutdown();
		return 1;
	}

	printf("tui: foundation\r\n");
	tui_shutdown();
	tui_shutdown();
	result = tui_end();
	if (result != -KOS_EINVAL) {
		printf("tui: FAIL end after shutdown %d\r\n", result);
		return 1;
	}
	result = tui_init();
	if (result != 0) {
		printf("tui: FAIL restart %d\r\n", result);
		return 1;
	}
	tui_shutdown();
	printf("tui: PASS\r\n");
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 0 || (argc == 1 && strcmp(argv[0], "foundation") == 0)) {
		return foundation();
	}
	printf("tui: FAIL mode\r\n");
	return 1;
}
