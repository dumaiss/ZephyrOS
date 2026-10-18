/* Private libtui storage.
 *
 * One definition shared by the lifecycle and transaction members.  A
 * program that links one member pulls only this storage and that member.
 */

#include "tui_internal.h"

struct tui_state tui_state;
