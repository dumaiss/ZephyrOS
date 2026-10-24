/* The wide-capacity outcome program's mode dispatcher.
 *
 * tui_wide.exe builds from the same tui_test.c but carries no
 * 85-column-only fast-path modes: the wide build's capacity leaves the
 * outcome program no room for them.
 */

#include "kos.h"

int tui_modes(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	return -1;
}
