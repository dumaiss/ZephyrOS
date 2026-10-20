/* Private libtui storage.
 *
 * One definition shared by the screen, lifecycle and transaction members:
 * the logical cell map, dirty/invert bitmaps, flush buffers and state. A
 * program links only the storage its called members need.
 */

#include "tui_internal.h"

struct tui_state tui_state;
struct tui_cell tui_cells[TUI_CELL_COUNT];
uint8_t tui_dirty[TUI_DIRTY_BYTES];
uint8_t tui_invert[TUI_DIRTY_BYTES];
uint8_t tui_run[TUI_RUN_MAX];
