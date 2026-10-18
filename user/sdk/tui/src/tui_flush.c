/* libtui transaction boundaries.
 *
 * tui_begin() increases a bounded nesting count.  tui_end() decreases it
 * and flushes only when it closes the outermost transaction.  tui_flush()
 * always flushes explicitly, inside a transaction too.
 */

#include "tui_internal.h"
#include "kos/console.h"

void tui_begin(void)
{
	if (tui_state.initialized == 0u) {
		return;
	}
	if (tui_state.transaction_depth < TUI_TRANSACTION_MAX) {
		++tui_state.transaction_depth;
	}
}

int tui_end(void)
{
	if (tui_state.initialized == 0u) {
		return -KOS_EINVAL;
	}
	if (tui_state.transaction_depth == 0u) {
		return -KOS_EINVAL;
	}
	--tui_state.transaction_depth;
	if (tui_state.transaction_depth != 0u) {
		return 0;
	}
	return console_flush();
}

int tui_flush(void)
{
	if (tui_state.initialized == 0u) {
		return -KOS_EINVAL;
	}
	return console_flush();
}
