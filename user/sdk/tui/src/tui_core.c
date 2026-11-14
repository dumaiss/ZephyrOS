/* Small shared lifecycle/key state. The cell arrays stay in tui_state.c,
 * so programs using only the VT100 decoder allocate no screen model. */
#include "tui_internal.h"
struct tui_state tui_state;
const struct tui_mouse_hook *tui_mouse_hook;
