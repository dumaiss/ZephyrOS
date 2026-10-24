/* The puttext equivalence mode on the 128 x 35 capacity
 * (TUI_GRID=128x35).  tui_wide.exe has no room left for it, so it is its
 * own program; the replay compares the two phases' render streams.
 */

#include "kos.h"

int puttext_mode(void);

int main(void)
{
	return puttext_mode();
}
